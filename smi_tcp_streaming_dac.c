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
#include <linux/broadcom/bcm2835_smi.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sched.h>

// Kernel 6.12 Fixes
#undef BCM2835_SMI_IOC_MAGIC
#undef BCM2835_SMI_IOC_WRITE_SETTINGS
#define BCM2835_SMI_IOC_MAGIC 0x01
#define BCM2835_SMI_IOC_WRITE_SETTINGS _IO(BCM2835_SMI_IOC_MAGIC, 1)

#define DATA_PORT 1234
#define CTRL_PORT 5000
#define BUFFER_SIZE (4 * 1024 * 1024)

// --- Sample-Paar-Swap (SMI-DMA-Packing) ------------------------------------
// WICHTIG: Analog zum RX-Pfad (smi_udp_streaming_adc.c) haengt es vom
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
#define NUM_BUFFERS 2

uint8_t *buffer_a, *buffer_b;
uint8_t *buffers[NUM_BUFFERS];      // Index 0 = buffer_a, Index 1 = buffer_b
int buf_busy[NUM_BUFFERS] = {0, 0}; // 1 = Puffer befindet sich gerade in der
                                     // Pipeline (wird gefüllt / wartet in der
                                     // Queue / wird per SMI geschrieben) und
                                     // darf vom Netzwerk-Thread NICHT
                                     // überschrieben werden.

int ready_queue[NUM_BUFFERS];  // FIFO der fertig gefüllten Pufferindizes
int queue_head = 0, queue_tail = 0, queue_count = 0;

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
        buf_busy[idx] = 0;
    }
    // Für den nächsten Client wieder bei Index 0 (buffer_a) beginnen.
    queue_head = 0;
    queue_tail = 0;
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
            while (buf_busy[fill_idx] && !stop) {
                pthread_cond_wait(&cond_free, &mutex);
            }
            if (stop) { pthread_mutex_unlock(&mutex); goto disconnect; }
            // Puffer ab sofort für Netzwerk-Thread reserviert.
            buf_busy[fill_idx] = 1;
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
                    buf_busy[fill_idx] = 0;
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

    smi_fd = open("/dev/smi", O_RDWR);
    buffer_a = malloc(BUFFER_SIZE); buffer_b = malloc(BUFFER_SIZE);

    if (!buffer_a || !buffer_b) {
        fprintf(stderr, "[ERROR] Speicherzuweisung für Puffer fehlgeschlagen!\n");
        if (buffer_a) free(buffer_a);
        if (buffer_b) free(buffer_b);
        close(smi_fd);
        return 1;
    }
    buffers[0] = buffer_a;
    buffers[1] = buffer_b;

    update_smi_settings(5, 16); // startup default 5 MSPS / 16 bit mode

    pthread_t net_t, ctrl_t;
    pthread_create(&net_t, NULL, network_thread, NULL);
    pthread_create(&ctrl_t, NULL, control_thread, NULL);

    while (!stop) {
        pthread_mutex_lock(&mutex);
        while (queue_count == 0 && !stop) pthread_cond_wait(&cond, &mutex);
        if (stop && queue_count == 0) { pthread_mutex_unlock(&mutex); break; }

        int idx = ready_queue[queue_head];
        queue_head = (queue_head + 1) % NUM_BUFFERS;
        queue_count--;
        pthread_mutex_unlock(&mutex);

        write(smi_fd, buffers[idx], BUFFER_SIZE);

        pthread_mutex_lock(&mutex);
        buf_busy[idx] = 0;
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

    if (buffer_a) {
        free(buffer_a);
        buffer_a = NULL;
    }

    if (buffer_b) {
        free(buffer_b);
        buffer_b = NULL;
    }

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
    pthread_cond_destroy(&cond_free);
    return 0;
}
