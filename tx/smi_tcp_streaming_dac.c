// _GNU_SOURCE wird für CPU_ZERO/CPU_SET/pthread_setaffinity_np
// benötigt (GNU-Erweiterungen von glibc, nicht Teil von POSIX). Muss vor
// allen System-Includes stehen.
#define _GNU_SOURCE

/*
 * SMI 16-Bit Pin-Belegung auf dem Raspberry Pi (GPIO):
 * --------------------------------------------------
 * Daten-Bits (D0-D15):
 * SD0  : GPIO 8  (Pin 24) | SD8  : GPIO 16 (Pin 36)
 * SD1  : GPIO 9  (Pin 21) | SD9  : GPIO 17 (Pin 11)
 * SD2  : GPIO 10 (Pin 19) | SD10 : GPIO 18 (Pin 12)
 * SD3  : GPIO 11 (Pin 23) | SD11 : GPIO 19 (Pin 35)
 * SD4  : GPIO 12 (Pin 32) | SD12 : GPIO 20 (Pin 38)
 * SD5  : GPIO 13 (Pin 33) | SD13 : GPIO 21 (Pin 40)
 * SD6  : GPIO 14 (Pin  8) | SD14 : GPIO 22 (Pin 15)
 * SD7  : GPIO 15 (Pin 10) | SD15 : GPIO 23 (Pin 16)
 *
 * Steuer-Signale:
 * SWE  : GPIO 7  (Pin 26) - SMI Write Enable (Taktet die Daten in den DAC)
 *
 * Hinweis:
 * Die GPIOs müssen ggf. auf die Alternate Function 1 'SMI' gesetzt werden.
 * Das 'smi-dev' Overlay übernimmt dies normalerweise beim Booten automatisch.
 */

/*
target-rate	cycles (total)	real-rate	error
5.0 MSPS	25	5.0000 MSPS		0% (ok)
6.25 MSPS	20	6.2500 MSPS		0% (ok)
10.0 MSPS	12.5	10.4167 MSPS		+4.1% (bad choice)
12.5 MSPS	10	12.5000 MSPS		0% (ok)
15.625 MSPS	8	15.6250 MSPS		0% (ok)
25.0 MSPS	5	25.0000 MSPS		0% (ok) */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
//#include <linux/broadcom/bcm2835_smi.h>
#include "../headers/bcm2835_smi.h"
#include <netinet/tcp.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h> // atomare Zähler für lockfreien Fast-Path
#include <sys/mman.h>  // mlockall() gegen Page-Faults im Hot-Path

// Kernel 6.12 Fixes
#undef BCM2835_SMI_IOC_MAGIC
#undef BCM2835_SMI_IOC_WRITE_SETTINGS
#define BCM2835_SMI_IOC_MAGIC 0x01
#define BCM2835_SMI_IOC_WRITE_SETTINGS _IO(BCM2835_SMI_IOC_MAGIC, 1)

#define DATA_PORT 1234
#define CTRL_PORT 5000
#define BUFFER_SIZE (32 * 1024 * 1024)

// --- Sample-Paar-Swap (SMI-DMA-Packing) ------------------------------------
// WICHTIG: Analog zum RX-Pfad haengt es vom
// SMI-Kernel-Treiber (bcm2835-smi) ab, ob dieser Swap noetig ist - NICHT
// direkt von der Hardware-Spezifikation. Laut Broadcom-Datenblatt
// (Abschnitt 6, PXLDAT=1/WFORMAT=0, 16-Bit) sollten Samples eigentlich in
// natuerlicher Reihenfolge uebertragen werden. Ob das hier im TX-Pfad
// (write() statt read()) genauso ist wie im RX-Pfad, ist NICHT automatisch
// gegeben - DMA-Packing kann bei Schreib- und Leserichtung unterschiedlich
// funktionieren. MUSS separat verifiziert werden (z.B. Rampen-Testmuster
// senden, mit Logikanalysator an D0-D15 pruefen oder GNU-Radio-Vergleich).
//
// -> Bei Aenderung von Kernel-/Treiberversion, Pi-Modell oder Wechsel
//    zwischen 8-/16-Bit-Modus IMMER neu verifizieren, bevor man sich auf
//    den aktuell hier gesetzten Wert verlaesst!
#define SWAP_SAMPLE_PAIRS 0    // Default aus, RX-Pfad nutzt denselben Treiber
                               // - Verhalten RX/TX auf dem konkreten System aber unbedingt separat gegenpruefen!

#if SWAP_SAMPLE_PAIRS
// Vertauscht Paare von Sample-Einheiten (unit_size Byte gross) in-place:
// [U0,U1,U2,U3,...] -> [U1,U0,U3,U2,...]. len MUSS ein ganzzahliges
// Vielfaches von (2*unit_size) sein (BUFFER_SIZE = 4 MiB erfuellt das
// fuer unit_size 1 und 2 problemlos).
static inline void swap_sample_pairs(uint8_t *buf, size_t len, size_t unit_size) {
    for (size_t i = 0; i + 2 * unit_size <= len; i += 2 * unit_size) {
        for (size_t b = 0; b < unit_size; b++) {
            uint8_t tmp = buf[i + b];
            buf[i + b] = buf[i + unit_size + b];
            buf[i + unit_size + b] = tmp;
        }
    }
}
#endif

// Zuletzt per "width"-Kommando gesetzte Bitbreite. Wird nur von
// control_thread geschrieben und von network_thread nur gelesen (analog zu
// g_applied_width im RX-Pfad). Anders als im RX-Pfad ist hier kein
// SCHED_FIFO-Echtzeit-Wettlauf um denselben fd zu beachten, ein einfacher
// volatile Read reicht: width-Wechsel sind selten, und ein kurzzeitig
// "veralteter" Wert waehrend der Umschaltung betrifft hoechstens einen
// einzelnen 4-MiB-Pufferzyklus.
volatile int g_current_width = 16;

// --- Doppelpuffer für Port 1234 -------------------------------------------
// Statt einer einzelnen active_buffer/buffer_ready-Variable (die bei
// schnellem Timing überschrieben werden konnte) verwenden wir eine echte
// Zwei-Slot-FIFO-Queue. Damit kann ein fertiger Puffer niemals verloren
// gehen, egal wie das Timing zwischen Netzwerk-Thread und Main-Thread
// ausfällt.
// GAPLESS-OPT: Von 2 auf 4 Puffer erhöht. Der Netzwerk-Thread bekommt dadurch
// mehr Vorlauf, sodass der Main-Thread (SMI-write) fast immer einen bereits
// fertig gefüllten Puffer in der Queue vorfindet und NICHT in den teuren
// cond_wait()-Schlafpfad muss (Aufwachlatenz durch Scheduler-Tick/IRQ ist die
// groesste Einzelquelle fuer Jitter zwischen zwei write()-Aufrufen).
// Kostet zusaetzlich 2*BUFFER_SIZE , auf dem Pi unkritisch.
#define NUM_BUFFERS 4

// GAPLESS-OPT: Dedizierte Cores fuer Main-Thread (SMI-Write) und die beiden
// Netzwerk-/Steuer-Threads. Per Kernel-Cmdline z.B. "isolcpus=2,3" reservieren,
// damit auf diesen Cores keine anderen Prozesse/IRQs den Main-Thread stoeren.
// Muss ggf. an das konkrete Pi-Modell (Anzahl Cores) angepasst werden.
#define MAIN_THREAD_CPU 3
#define NET_THREAD_CPU  2

uint8_t *buffers[NUM_BUFFERS];      // Zeiger auf alle NUM_BUFFERS Puffer,
                                     // werden in main() alloziert (siehe dort)

// GAPLESS-OPT: buf_busy von "int" auf "atomic_int" umgestellt. Main-Thread
// setzt dies nach dem write() zurueck OHNE Mutex zu halten (Punkt 1 des
// Plans) - dafuer muss der Zugriff selbst atomar/race-frei sein. Der
// Netzwerk-Thread liest ihn weiterhin innerhalb der mutex-geschuetzten
// cond_wait-Schleife (das ist fuer die Wait/Signal-Korrektheit noetig,
// siehe Kommentar bei cond_free weiter unten) - "atomic" schadet dort nicht,
// es ist nur zusaetzliche Sicherheit gegen Teil-Reads/-Writes.
atomic_int buf_busy[NUM_BUFFERS]; // 1 = Puffer befindet sich gerade in der
                                   // Pipeline (wird gefüllt / wartet in der
                                   // Queue / wird per SMI geschrieben) und
                                   // darf vom Netzwerk-Thread NICHT
                                   // überschrieben werden.

int ready_queue[NUM_BUFFERS];  // FIFO der fertig gefüllten Pufferindizes.
                                // WICHTIG (Voraussetzung fuer den lockfreien
                                // Fast-Path unten): Es gibt genau einen
                                // Producer (network_thread, schreibt nur
                                // queue_tail) und genau einen Consumer
                                // (main(), liest/schreibt nur queue_head).
                                // Das ist ein Single-Producer/Single-Consumer
                                // Ring - deshalb ist ein lockfreier Read von
                                // queue_head/ready_queue[] durch main() sicher,
                                // OHNE dass dies ein generisches "lockfrei fuer
                                // beliebige Zugriffsmuster" waere.
int queue_head = 0, queue_tail = 0, queue_count = 0;

// GAPLESS-OPT: Spiegelt queue_count atomar, damit main() OHNE Mutex prüfen
// kann, ob bereits ein fertiger Puffer wartet (Fast-Path). Die mutex-
// geschützten Variablen (queue_count, queue_head, ready_queue) bleiben die
// "Wahrheit" für den Schlafpfad (cond_wait) - queue_count_a ist nur ein
// schneller Vorab-Check, um den teuren Mutex im Normalfall zu vermeiden.
atomic_int queue_count_a;

int smi_fd;

volatile sig_atomic_t stop = 0;
int sig_count = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_free = PTHREAD_COND_INITIALIZER; // Signal für "Puffer frei"
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;      // Signal für "Puffer in Queue"


// Hilfsfunktion: Core-Frequenz messen
long get_core_freq() {
    FILE *fp = popen("vcgencmd measure_clock core", "r");
    char res[64];
    if (fp && fgets(res, sizeof(res), fp)) {
        pclose(fp);
        char *p = strchr(res, '=');
        return p ? atol(p + 1) : 250000000;
    }
    return 250000000;
}

// Zentrale Funktion zum Setzen der SMI-Hardware
void update_smi_settings(float msps, int width) {
    struct smi_settings settings = {0};

    long core_f = get_core_freq();
    printf("core_f=%ld\n",core_f);

    // Faktor 2 Korrektur für Pi 4 / 16-Bit Modus
    int smi_divisor = 2;
    int total_cycles = (int)((float)core_f / (msps * 1000000.0f * (float)smi_divisor) + 0.5f);

    //int total_cycles = core_f / (msps * 1000000);
    //if (total_cycles < 4) total_cycles = 4;

    if (total_cycles < 3) total_cycles = 3;

    // Phasen berechnen
    int setup = total_cycles / 4;
    int hold = total_cycles / 4;

    // WICHTIG: Strobe bekommt den Rest, damit keine Zyklen durch Abrunden verloren gehen
    int strobe = total_cycles - setup - hold;

    // Sicherheitscheck: Jede Phase muss mindestens 1 Takt lang sein
    if (setup == 0) setup = 1;
    if (hold == 0) hold = 1;
    if (strobe <= 0) strobe = 1;

    settings.data_width = (width == 16) ? 1 : 0; // 0=8bit, 1=16bit
    settings.pack_data = 1;
    settings.write_setup_time = setup;
    settings.write_strobe_time = strobe;
    settings.write_hold_time = hold;
    settings.dma_enable = 1;
    settings.dma_write_thresh = 63;
    settings.dma_panic_write_thresh = 32;

    // Sicherstellen, dass mindestens 1 Zyklus pro Phase bleibt
    if (settings.write_strobe_time == 0) settings.write_strobe_time = 1;

    //settings.write_pace_time = 0;    //settings.read_pace_time = 0;

    if (ioctl(smi_fd, BCM2835_SMI_IOC_WRITE_SETTINGS, &settings) == 0) {
        // Ausgabe der echten Rate zur Kontrolle am Terminal
        double real_msps = (double)core_f / (total_cycles * smi_divisor * 1000000.0);
        printf("[CTRL] Update: Ziel %.2f MSPS -> Real %.4f MSPS (Cycles: %d [%d/%d/%d])\n",
               msps, real_msps, total_cycles, setup, strobe, hold);
    }
}


// Thread: Control Port 5000 (Klartext)
void *control_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(CTRL_PORT) };
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    float cur_rate = 5.0f;
    int cur_width = 16;
    while (!stop) {
        int client = accept(server_fd, NULL, NULL);
        char cmd[64] = {0};
        read(client, cmd, sizeof(cmd)-1);

        if (strncmp(cmd, "rate ", 5) == 0) {
            float val = atof(cmd + 5);
            if (val > 0.1f && val <= 40.0f) { // Plausibilitätsprüfung!
                cur_rate = val;
                update_smi_settings(cur_rate, cur_width);
            }
        } else if (strncmp(cmd, "width ", 6) == 0) {
            int w = atoi(cmd + 6);
            if (w == 8 || w == 16) {
                cur_width = w;
                g_current_width = w; // fuer swap_sample_pairs() im network_thread
                update_smi_settings(cur_rate, cur_width);
            }
        }
        close(client);
    }
    return NULL;
}

// --- Queue-Hilfsfunktionen (nur für Port-1234-Pipeline) --------------------
// Müssen unter Haltung von 'mutex' aufgerufen werden.

// Fertig gefüllten Puffer in die Queue einreihen und Main-Thread wecken.
static void queue_push_locked(int idx) {
    ready_queue[queue_tail] = idx;
    queue_tail = (queue_tail + 1) % NUM_BUFFERS;
    queue_count++;
    // GAPLESS-OPT: atomaren Spiegelzähler NACH dem eigentlichen Push
    // aktualisieren (release-Semantik), damit main() ihn lockfrei lesen kann.
    atomic_store_explicit(&queue_count_a, queue_count, memory_order_release);
    pthread_cond_signal(&cond);
}

// Beim Disconnect: alle noch NICHT von main() abgeholten Queue-Einträge
// verwerfen und deren busy-Flag wieder freigeben. Ein Puffer, den main()
// bereits aus der Queue entnommen hat (und der evtl. gerade per SMI-DMA
// geschrieben wird), steht NICHT mehr in der Queue und wird hier deshalb
// nicht angefasst -- das verhindert genau die Race Condition, bei der ein
// neu verbundener Client in einen noch aktiven DMA-Puffer schreibt.
static void queue_discard_pending_locked(void) {
    while (queue_count > 0) {
        int idx = ready_queue[queue_head];
        queue_head = (queue_head + 1) % NUM_BUFFERS;
        queue_count--;
        atomic_store_explicit(&buf_busy[idx], 0, memory_order_release);
    }
    // Für den nächsten Client wieder bei Index 0 (buffer_a) beginnen.
    queue_head = 0;
    queue_tail = 0;
    // GAPLESS-OPT: atomaren Spiegelzähler mit zurücksetzen (queue_count ist
    // an dieser Stelle bereits 0, siehe while-Schleife oben).
    atomic_store_explicit(&queue_count_a, 0, memory_order_release);
}

// Thread: Data Port 1234 (Stream)
void *network_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket Fehler");
        return NULL;
    }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DATA_PORT) };

    int opt = 1;
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind Fehler");
        close(server_fd);
        return NULL;
    }

    listen(server_fd, 1);
     printf("[DATA] Warte auf Netzwerk-Stream auf Port %d...\n", DATA_PORT);

    while (!stop) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        printf("[DATA] Client verbunden!\n");

        // --- Socket-Tuning ---
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int rcvbuf = 2 * 1024 * 1024; // 2MB Kernel-Empfangspuffer
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        int fill_idx = 0; // immer mit buffer_a (Index 0) beginnen
        while (!stop) {

            // 1. WARTEN: Ist der Puffer, den ich füllen will, noch in Benutzung
            //    (in der Queue, oder gerade per SMI-DMA am Schreiben)?
            //    Solange TCP hier blockiert, entsteht ganz natürlich die
            //    gewünschte Backpressure Richtung GNU Radio.
            pthread_mutex_lock(&mutex);
            // GAPLESS-OPT: atomic_load statt einfachem int-Read - der Wert
            // wird jetzt auch lockfrei vom Main-Thread geschrieben (siehe
            // dort), daher muss der Read hier atomar/race-frei erfolgen.
            // Die Wait/Signal-Logik selbst bleibt unverändert mutex-basiert:
            // pthread_cond_wait() verlangt, dass Prüfen der Bedingung UND
            // das Einschlafen atomar bzgl. desselben Mutex passieren, sonst
            // drohen verlorene Wakeups. Der Main-Thread sendet sein Signal
            // erst NACHDEM er denselben Mutex genommen hat (siehe main()),
            // daher bleibt das Zusammenspiel korrekt.
            while (atomic_load_explicit(&buf_busy[fill_idx], memory_order_acquire) && !stop) {
                pthread_cond_wait(&cond_free, &mutex);
            }
            if (stop) { pthread_mutex_unlock(&mutex); goto disconnect; }
            // Puffer ab sofort für Netzwerk-Thread reserviert.
            atomic_store_explicit(&buf_busy[fill_idx], 1, memory_order_release);
            pthread_mutex_unlock(&mutex);

            // 2. FÜLLEN: Daten vom Netzwerk lesen (kein anderer Thread
            //    greift auf diesen Puffer zu, solange busy[fill_idx]==1
            //    und er noch nicht in der Queue steht -> unkritisch ohne Lock).
            uint8_t *fill_ptr = buffers[fill_idx];
            size_t rx = 0;
            while (rx < BUFFER_SIZE) {
                ssize_t n = read(client_fd, fill_ptr + rx, BUFFER_SIZE - rx);
                if (n <= 0) {
                    printf("[DATA] Client getrennt.\n");
                    // Unvollständig gefüllten Puffer nicht in die Queue
                    // einreihen, sondern busy-Flag sofort wieder freigeben.
                    pthread_mutex_lock(&mutex);
                    atomic_store_explicit(&buf_busy[fill_idx], 0, memory_order_release);
                    pthread_mutex_unlock(&mutex);
                    goto disconnect;
                }
                rx += n;
            }

#if SWAP_SAMPLE_PAIRS
            // Direkt nach vollstaendigem Empfang und VOR dem Queue-Push,
            // damit der zeitkritische write(smi_fd, ...) im Main-Thread
            // nicht zusaetzlich belastet wird. unit_size an aktuelle Breite
            // koppeln, NICHT hartkodieren - sonst wird bei width=8
            // fehlerhaft geswappt (siehe RX-Pfad, g_applied_width).
            {
                size_t unit_size = (g_current_width == 16) ? 2 : 1;
                swap_sample_pairs(fill_ptr, BUFFER_SIZE, unit_size);
            }
#endif

            // 3. SIGNAL: Puffer fertig -> in Queue einreihen, Main-Thread weckt auf.
            pthread_mutex_lock(&mutex);
            queue_push_locked(fill_idx);
            pthread_mutex_unlock(&mutex);

            // Für den nächsten Durchlauf zum anderen Puffer wechseln.
            fill_idx = (fill_idx + 1) % NUM_BUFFERS;
        }
        disconnect: close(client_fd);
        // Nach Disconnect: nur die Puffer freigeben, die NICHT gerade beim
        // Main-Thread (SMI-Schreibvorgang) in Arbeit sind. Dadurch wird
        // niemals in einen laufenden DMA-Puffer hineingeschrieben.
        pthread_mutex_lock(&mutex);
        queue_discard_pending_locked();
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

void handle_sigint(int sig) {
    sig_count++;

    if (sig_count == 1) {
      stop = 1;
    } else {
      _exit(1);
    }
}

int main() {
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == -1) {
        perror("Warnung: Konnte Real-Time Priorität nicht setzen (sudo vergessen?)");
    }

    struct sigaction sa;
    sa.sa_handler = &handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // GAPLESS-OPT: Main-Thread (führt den zeitkritischen write(smi_fd,...)
    // aus) an einen dedizierten Core binden. Reduziert Jitter durch
    // Scheduler-Migration/IRQ-Verdrängung zwischen zwei write()-Aufrufen.
    // Voraussetzung, damit das wirklich isoliert ist: Kernel-Cmdline z.B.
    // "isolcpus=2,3" - sonst kann der Scheduler weiterhin andere Prozesse
    // auf diesen Core legen.
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(MAIN_THREAD_CPU, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
            perror("Warnung: Konnte Main-Thread nicht an CPU pinnen");
        }
    }

    // GAPLESS-OPT: Gesamten Prozessspeicher (inkl. künftiger Allokationen)
    // gegen Swapping sperren. Verhindert, dass ausgerechnet im Übergang
    // zwischen zwei write()-Aufrufen ein Page-Fault (z.B. beim ersten
    // Zugriff auf einen frisch gemappten Puffer) die Rückkehr in den
    // DMA-Start verzögert.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("Warnung: mlockall() fehlgeschlagen (evtl. keine Root-Rechte)");
    }

    smi_fd = open("/dev/smi", O_RDWR);

    // BUG-FIX: Vorher wurden nur buffer_a/buffer_b (2 Puffer) alloziert und
    // auf buffers[0]/buffers[1] gelegt, obwohl NUM_BUFFERS=4 gesetzt ist.
    // buffers[2] und buffers[3] blieben dadurch NULL (globales Array im
    // BSS-Segment) - network_thread() zaehlt fill_idx aber bis NUM_BUFFERS-1
    // hoch und hat bei fill_idx==2 durch einen NULL-Pointer geschrieben
    // (read() in buffers[2]+rx = NULL+0). Das fuehrte zum Absturz des
    // Servers nach wenigen Sekunden (genau das beobachtete "Client
    // disconnect"-Symptom). Fix: ALLE NUM_BUFFERS Puffer allozieren.
    int alloc_failed = 0;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = malloc(BUFFER_SIZE);
        if (!buffers[i]) alloc_failed = 1;
    }

    if (alloc_failed) {
        fprintf(stderr, "[ERROR] Speicherzuweisung für Puffer fehlgeschlagen!\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (buffers[i]) { free(buffers[i]); buffers[i] = NULL; }
        }
        close(smi_fd);
        return 1;
    }

    // GAPLESS-OPT: buf_busy[] ist jetzt atomic_int und braucht eine explizite
    // Initialisierung (der alte "= {0, 0}"-Initialisierer entfällt durch die
    // Umstellung auf NUM_BUFFERS=4 ohnehin).
    for (int i = 0; i < NUM_BUFFERS; i++) {
        atomic_init(&buf_busy[i], 0);
    }
    atomic_init(&queue_count_a, 0);

    update_smi_settings(5, 16); // startup default 5 MSPS / 16 bit mode

    pthread_t net_t, ctrl_t;
    pthread_create(&net_t, NULL, network_thread, NULL);
    pthread_create(&ctrl_t, NULL, control_thread, NULL);

    // GAPLESS-OPT: Netzwerk-Thread auf einen anderen Core als der Main-
    // Thread legen, damit er dessen Cache/Scheduling-Slot nicht stört.
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(NET_THREAD_CPU, &cpuset);
        if (pthread_setaffinity_np(net_t, sizeof(cpuset), &cpuset) != 0) {
            perror("Warnung: Konnte Netzwerk-Thread nicht an CPU pinnen");
        }
    }

    // GAPLESS-OPT: Kern der Optimierung. Ablauf pro Iteration:
    //
    //   1. Puffer-Index holen - im Normalfall (Queue nicht leer) OHNE Mutex,
    //      da queue_head/ready_queue[] als Single-Producer/Single-Consumer-
    //      Ring nur von main() gelesen UND geschrieben werden (siehe
    //      Kommentar bei den Variablendeklarationen). Nur der Vorab-Check
    //      "ist die Queue ueberhaupt leer?" laeuft ueber den atomaren
    //      Spiegelzaehler queue_count_a - dafuer ist kein Mutex noetig.
    //   2. write(smi_fd, ...) - der eigentliche DMA-Transfer.
    //   3. Freigabe des Puffers OHNE Mutex (atomic_store), damit zwischen
    //      write()-Rueckkehr und dem naechsten Schleifendurchlauf
    //      (naechstes write()) kein Lock-Erwerb mehr im Weg steht.
    //      Der Mutex wird NUR noch angefasst, um pthread_cond_signal()
    //      korrekt gegen den network_thread abzusichern (siehe Kommentar
    //      dort) - das liegt aber NACH dem Signalisieren des freien Puffers
    //      und blockiert damit nicht mehr den naechsten write()-Start.
    //
    //   Der teure Pfad (echtes pthread_cond_wait mit Mutex) wird nur noch
    //   durchlaufen, wenn wirklich kein fertiger Puffer vorliegt - im
    //   Dauerbetrieb mit NUM_BUFFERS=4 sollte das die Ausnahme sein.
    while (!stop) {
        int idx;

        if (atomic_load_explicit(&queue_count_a, memory_order_acquire) == 0) {
            // Slow path: Queue ist (vermutlich) leer -> regulaerer,
            // mutex-geschuetzter Schlafpfad wie zuvor.
            pthread_mutex_lock(&mutex);
            while (queue_count == 0 && !stop) pthread_cond_wait(&cond, &mutex);
            if (stop && queue_count == 0) { pthread_mutex_unlock(&mutex); break; }

            idx = ready_queue[queue_head];
            queue_head = (queue_head + 1) % NUM_BUFFERS;
            queue_count--;
            atomic_store_explicit(&queue_count_a, queue_count, memory_order_release);
            pthread_mutex_unlock(&mutex);
        } else {
            // Fast path: Es liegt bereits ein fertiger Puffer vor.
            // queue_head/ready_queue[] duerfen hier lockfrei gelesen werden,
            // weil ausschliesslich main() sie konsumiert (SPSC-Ring, siehe
            // oben) - der Producer (network_thread) fasst nur queue_tail an.
            idx = ready_queue[queue_head];
            queue_head = (queue_head + 1) % NUM_BUFFERS;
            atomic_fetch_sub_explicit(&queue_count_a, 1, memory_order_acq_rel);
        }

        // --- Zeitkritischer Abschnitt: hier darf zwischen zwei
        //     write()-Aufrufen möglichst NICHTS mehr dazwischenliegen ---
        write(smi_fd, buffers[idx], BUFFER_SIZE);

        // Puffer sofort freigeben, BEVOR der Mutex fuer das cond_signal
        // genommen wird - der network_thread darf den Puffer ab hier wieder
        // befuellen, auch wenn er den Mutex fuer sein eigenes Aufwachen noch
        // nicht bekommen hat.
        atomic_store_explicit(&buf_busy[idx], 0, memory_order_release);

        // pthread_cond_signal() OHNE gehaltenen Mutex aufzurufen ist laut
        // POSIX zwar erlaubt, birgt hier aber ein Lost-Wakeup-Risiko: Der
        // network_thread prueft buf_busy und geht in cond_wait() unter
        // Haltung von 'mutex' (siehe dort). Nehmen wir den Mutex hier NICHT,
        // koennte folgendes Race auftreten: network_thread sieht buf_busy==1
        // (Store oben noch nicht sichtbar/Reihenfolge ungluecklich),
        // haengt sich in cond_wait ein - und unser Signal davor geht
        // verloren, weil noch niemand wartet. Deshalb bleibt das Signal an
        // den Mutex gekoppelt; es liegt aber NACH dem eigentlichen
        // Freigeben und NACH dem write(), blockiert also nicht mehr den
        // naechsten Schleifendurchlauf/write()-Start.
        pthread_mutex_lock(&mutex);
        pthread_cond_signal(&cond_free); // Wecke Netzwerk-Thread
        pthread_mutex_unlock(&mutex);
    }

    pthread_cond_broadcast(&cond_free);
    pthread_cond_broadcast(&cond);

    pthread_join(net_t, NULL);
    pthread_join(ctrl_t, NULL);

    if (smi_fd >= 0) {
        if (close(smi_fd) == -1) {
            perror("[CLEANUP] close(smi_fd)");
        }
        smi_fd = -1;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (buffers[i]) {
            free(buffers[i]);
            buffers[i] = NULL;
        }
    }

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
    pthread_cond_destroy(&cond_free);
    return 0;
}
