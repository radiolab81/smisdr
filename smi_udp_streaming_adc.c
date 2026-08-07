/*
 * smi_udp_streaming_adc.c
 * -----------
 * Empfangszweig zu smisdr: liest 16-Bit-Samples per SMI/DMA von einem
 * ADC und sendet sie per UDP an einen PC (GNU Radio). Der Steuerkanal auf
 * Port 5000 bleibt kompatibel zur Senderichtung (rate/width), wurde aber
 * um "dest <ip> <port>" erweitert, da der Pi hier aktiv sendet statt auf
 * eine Verbindung zu warten.
 *
 * SMI 16-Bit Pin-Belegung: siehe smisdr - tx(identisch, nur Datenrichtung
 * der Steuerleitung ist logisch anders - SRE statt SWE, siehe Hinweis unten).
 * SMI 16-Bit Pin-Belegung auf dem Raspberry Pi (GPIO):
 * --------------------------------------------------
 * Daten-Bits (D0-D15):
 * SD0  : GPIO 8  (Pin 24) | SD8  : GPIO 16 (Pin 36)
 * SD1  : GPIO 9  (Pin 21) | SD9  : GPIO 17 (Pin 11)
 * SD2  : GPIO 10 (Pin 19) | SD10 : GPIO 18 (Pin 12)
 * SD3  : GPIO 11 (Pin 23) | SD11 : GPIO 19 (Pin 35)
 * SD4  : GPIO 12 (Pin 32) | SD12 : GPIO 20 (Pin 38)
 * SD5  : GPIO 13 (Pin 33) | SD13 : GPIO 21 (Pin 40)
 * SD6  : GPIO 14 (Pin  8) | SD14 : GPIO 24 (Pin 15)
 * SD7  : GPIO 15 (Pin 10) | SD15 : GPIO 25 (Pin 26)
 *
 * Hinweis SRE (SMI Read Enable):
 * Analog zu SWE (GPIO 7 / Pin 26) auf der Senderseite nutzt der ADC-Betrieb
 * das SMI-eigene OE/Read-Strobe-Signal, das vom Treiber automatisch über
 * dieselbe Alternate-Function-1-Konfiguration bereitgestellt wird. Es ist
 * keine gesonderte GPIO-Verkabelung nötig, sofern das 'smi-dev' Overlay
 * beim Booten geladen wird.
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
#include <stdarg.h>
#include <string.h>
#include <errno.h>
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
#include <time.h>

// Kernel 6.12 Fixes (identisch zur Senderseite)
#undef BCM2835_SMI_IOC_MAGIC
#undef BCM2835_SMI_IOC_WRITE_SETTINGS
#define BCM2835_SMI_IOC_MAGIC 0x01
#define BCM2835_SMI_IOC_WRITE_SETTINGS _IO(BCM2835_SMI_IOC_MAGIC, 1)

#define DATA_PORT_RX  1233 // Alternative: 1235 - einfach hier anpassen -> 1234 ist für die Gegenrichtung (Aussenden von HF) angedacht.
#define CTRL_PORT 5000

// WICHTIG (Diagnose Wasserfall-Aussetzer):
// Mit 4 MiB im vgl. zum TX, entsprach EIN Akquise-/Sende-Zyklus bei 5 MSPS/8-Bit ca. 0,84s
// Daten, die dann als ein einziger Burst von ~2850 UDP-Fragmenten
// moeglichst schnell rausgefeuert wurden - dazwischen kam ueber fast eine
// ganze Sekunde schlicht NICHTS an. Das uebersteigt leicht den
// Standard-UDP-Empfangspuffer unter Linux (net.core.rmem_default, oft nur
// ~208 KB) UND GNU Radios eigene interne Queue im udp_source-Block, sobald
// downstream (Qt-Waterfall/FFT) mal kurz nicht sofort hinterherkommt -
// Ergebnis: Paketverlust ohne dass unser Programm (auf dem Pi) davon
// ueberhaupt etwas mitbekommt ("verworfen=0" in den STATS trotzdem
// sichtbarer Luecke im Wasserfall).
//
// Mit 64 KiB liegt ein Zyklus bei 5 MSPS/8-Bit nur noch bei ~13ms Daten -
// das verhaelt sich fast wie ein echter Dauerstrom statt eines Bursts,
// bleibt bequem innerhalb typischer Default-Puffergroessen und senkt die
// Latenz der Live-Anzeige nebenbei von ~0,84s auf ~13ms. Bei Bedarf weiter
// anpassbar; NUM_BUFFERS (4 Slots) ergibt damit eine Gesamt-Pufferung von
// nur noch ~256 KiB / ~52ms - sollte fuer normale Netzwerk-Jitter reichen,
// laesst sich aber bei Bedarf ebenfalls erhoehen.
#define BUFFER_SIZE (64 * 1024)

// UDP-Fragmentierung: 1500 (Standard-MTU) - 20 (IP) - 8 (UDP) = 1472 nutzbar.
// KEIN eigenes Framing/Header: dieses Programm reicht den rohen HF-Bitstrom
// (8/10/12/14/16 Bit je nach Konfiguration) 1:1 durch, damit GNU Radios
// Standard-"UDP Source"-Block ihn direkt ohne Zusatzprotokoll konsumieren
// kann. IQ-Verarbeitung/In-Band-Signaling passiert in einer eigenen Schicht
// oberhalb dieses Programms und ist hier bewusst nicht implementiert.
// Konsequenz: Ein verlorenes UDP-Datagramm ist auf dieser Ebene weder
// erkennbar noch kompensierbar - der Bitstrom verschiebt sich in diesem
// Fall stillschweigend. Das ist hier explizit akzeptiertes Verhalten.
#define UDP_PAYLOAD   1472

// --- Ringpuffer für den ADC->Netzwerk Pfad ---------------------------------
// 4 Slots statt 2 wie beim TX: gibt dem UDP-Sende-Thread etwas Puffer, bevor
// die Akquise (die NIE warten darf, da die Hardware nicht pausierbar ist)
// anfangen muss, alte, noch unversendete Daten zu verwerfen.
#define NUM_BUFFERS 4

uint8_t *buffers[NUM_BUFFERS];
int buf_busy[NUM_BUFFERS] = {0}; // 1 = Slot ist belegt (in Queue ODER wird gerade versendet)

int ready_queue[NUM_BUFFERS];
int queue_head = 0, queue_tail = 0, queue_count = 0;

uint64_t stat_buffers_acquired = 0;
uint64_t stat_buffers_dropped  = 0; // wegen zu langsamem Netzwerk verworfen
uint64_t stat_buffers_sent     = 0;

int smi_fd = -1;
int server_fd = -1; // wird beim Shutdown aktiv geschlossen, um accept() zu befreien

volatile sig_atomic_t stop = 0;

pthread_mutex_t mutex        = PTHREAD_MUTEX_INITIALIZER; // schützt Ringpuffer-Status
pthread_mutex_t smi_io_mutex = PTHREAD_MUTEX_INITIALIZER; // serialisiert read()/ioctl() auf smi_fd (nur noch innerhalb von smi_thread relevant, siehe unten)
pthread_mutex_t dest_mutex   = PTHREAD_MUTEX_INITIALIZER; // schützt Ziel-Adresse
pthread_cond_t  cond_free    = PTHREAD_COND_INITIALIZER;  // "ein Slot wurde frei"
pthread_cond_t  cond_ready   = PTHREAD_COND_INITIALIZER;  // "ein Slot liegt fertig in der Queue"

struct sockaddr_in dest_addr;
int dest_configured = 0;

// --- Settings-Handoff (Fix fuer Priority Inversion) ------------------------
// FRUEHER: control_thread rief update_smi_settings() -> ioctl(smi_fd, ...)
// direkt auf und musste sich dafuer smi_io_mutex mit dem Echtzeit-Thread
// smi_thread teilen, der denselben Mutex fuer jeden read() haelt. Da
// smi_thread mit SCHED_FIFO 99 laeuft und den Mutex nach jeder Puffer-
// Akquise sofort wieder sperrt, ist das Zeitfenster, in dem der normal
// priorisierte control_thread ihn ergattern kann, verschwindend klein -
// ohne Prioritaets-Vererbung (PTHREAD_PRIO_INHERIT) kann das zu praktisch
// unbegrenztem Verhungern fuehren (beobachtet: rate/width haengen bis zum
// Shutdown).
//
// JETZT: smi_fd (sowohl read() als auch ioctl()) gehoert exklusiv
// smi_thread. control_thread setzt per settings_mutex (wird nie lange
// gehalten) nur noch Wunschwerte + ein dirty-Flag; smi_thread prueft das
// in der kurzen Luecke zwischen zwei Puffer-Akquisen und wendet die
// Einstellung selbst an. Damit gibt es gar keine Cross-Thread-Konkurrenz
// mehr um smi_fd, und control_thread kann strukturell nicht mehr
// unbegrenzt haengen (Wartezeit ist durch pthread_cond_timedwait
// hart begrenzt).
pthread_mutex_t settings_mutex        = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  settings_applied_cond = PTHREAD_COND_INITIALIZER;
int   settings_dirty    = 0;
float pending_rate       = 5.0f;
int   pending_width      = 16;
int   settings_apply_seq = 0;   // wird bei jeder Anwendung erhoeht
double last_real_msps    = 0.0;

// ---------------------------------------------------------------------------

// Core-Frequenz wird NUR EINMAL beim Programmstart gemessen (siehe main(),
// noch bevor irgendein Thread Echtzeitprioritaet traegt) und hier
// zwischengespeichert. Grund: popen()/vcgencmd forkt eine Shell; wuerde das
// aus einem Thread mit SCHED_FIFO-Prioritaet heraus wiederholt passieren
// (z.B. bei jedem "rate"/"width"-Kommando), kann der geforkte Kindprozess
// nicht mehr zuverlaessig gegen den konkurrierenden Echtzeit-Akquise-Thread
// auf die CPU kommen und im schlimmsten Fall auf unbestimmte Zeit haengen -
// inklusive gehaltenem smi_io_mutex, was dann den GESAMTEN Control-Kanal
// dauerhaft blockiert. Mit "recalib" kann die Messung bei Bedarf gezielt
// erneut angestossen werden (siehe control_thread).
long g_core_freq = 250000000;

long get_core_freq() {
    FILE *fp = popen("vcgencmd measure_clock core", "r");
    if (!fp) return 250000000;
    char res[64] = {0};
    long freq = 250000000;
    if (fgets(res, sizeof(res), fp)) {
        char *p = strchr(res, '=');
        if (p) freq = atol(p + 1);
    }
    pclose(fp); // IMMER schließen, auch wenn fgets() fehlschlägt (Fix ggü. TX-Vorlage)
    return freq;
}

// Setzt die SMI-Timings für den READ-Pfad. Analog zur Senderseite, aber
// mit read_setup/read_strobe/read_hold statt write_*. Die write_*-Felder
// werden - wie es auch der Kernel-Treiber selbst per Default tut
// (smi_get_default_settings) - auf dieselben Werte gespiegelt. Das ist
// unschädlich, da wir write() auf diesem fd nie aufrufen, verhindert aber
// undefinierte/leere Werte, falls der Treiber beim Setzen der Register
// dennoch beide Seiten konsistent prüft.
void update_smi_settings(float msps, int width) {
    pthread_mutex_lock(&smi_io_mutex);

    struct smi_settings settings = {0};

    // Keine erneute Shell-/vcgencmd-Messung hier mehr - siehe Kommentar bei
    // g_core_freq oben. Nur die einmalig beim Start gemessene, gecachte
    // Frequenz verwenden (oder eine ueber "recalib" aktualisierte).
    long core_f = g_core_freq;
    printf("[CTRL] core_f=%ld (gecacht)\n", core_f);

    // Faktor 2 Korrektur für Pi 4 / 16-Bit Modus
    int smi_divisor = 2; // gleiche empirische Pi4/16-Bit-Korrektur wie im Sendezweig
    int total_cycles = (int)((float)core_f / (msps * 1000000.0f * (float)smi_divisor) + 0.5f);
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

    settings.read_setup_time  = setup;
    settings.read_strobe_time = strobe;
    settings.read_hold_time   = hold;
    settings.read_pace_time   = 1;

    // Spiegelung auf write_* (siehe Funktionskommentar oben)
    settings.write_setup_time  = settings.read_setup_time;
    settings.write_strobe_time = settings.read_strobe_time;
    settings.write_hold_time   = settings.read_hold_time;
    settings.write_pace_time   = settings.read_pace_time;

    settings.dma_enable = 1;
    settings.dma_read_thresh = 1;         // klein halten: DMA soll früh anspringen
    settings.dma_panic_read_thresh = 0x20;

    if (smi_fd >= 0 && ioctl(smi_fd, BCM2835_SMI_IOC_WRITE_SETTINGS, &settings) == 0) {
        // Ausgabe der echten Rate zur Kontrolle am Terminal
        double real_msps = (double)core_f / (total_cycles * smi_divisor * 1000000.0);
        printf("[CTRL] Update: Ziel %.2f MSPS -> Real %.4f MSPS (Cycles: %d [%d/%d/%d])\n",
               msps, real_msps, total_cycles, setup, strobe, hold);
        pthread_mutex_lock(&settings_mutex);
        last_real_msps = real_msps;
        pthread_mutex_unlock(&settings_mutex);
    } else if (smi_fd >= 0) {
        perror("[CTRL] ioctl(BCM2835_SMI_IOC_WRITE_SETTINGS)");
    }
    fflush(stdout);

    pthread_mutex_unlock(&smi_io_mutex);
}

// --- Ringpuffer-Hilfsfunktionen (unter Haltung von 'mutex' aufzurufen) -----

static void queue_push_locked(int idx) {
    ready_queue[queue_tail] = idx;
    queue_tail = (queue_tail + 1) % NUM_BUFFERS;
    queue_count++;
    pthread_cond_signal(&cond_ready);
}

// --- Signal-Handling-Thread -------------------------------------------------
// Statt eines klassischen Signal-Handlers (der nur async-signal-safe
// Funktionen aufrufen darf, siehe Analyse zur Senderseite) blockieren wir
// SIGINT/SIGTERM in ALLEN Threads und fangen sie hier per sigwait() in
// einem regulären Thread-Kontext ab. Dort dürfen wir ganz normal
// pthread_cond_broadcast() und close() aufrufen - kein Henne-Ei-Problem,
// kein "zweimal Strg+C -> _exit()"-Notbehelf mehr nötig.
void *signal_thread(void *arg) {
    sigset_t *set = (sigset_t *)arg;
    int sig;
    sigwait(set, &sig);

    printf("\n[SIGNAL] Signal %d empfangen, fahre sauber herunter...\n", sig);
    stop = 1;

    // Alle wartenden Threads aufwecken:
    pthread_mutex_lock(&mutex);
    pthread_cond_broadcast(&cond_free);
    pthread_cond_broadcast(&cond_ready);
    pthread_mutex_unlock(&mutex);

    // Falls control_thread gerade in wait_for_settings_applied() haengt:
    // sofort aufwecken statt auf das 3s-Timeout zu warten.
    pthread_mutex_lock(&settings_mutex);
    pthread_cond_broadcast(&settings_applied_cond);
    pthread_mutex_unlock(&settings_mutex);

    // control_thread hängt evtl. in accept() - das reagiert auf keine
    // Condition-Variable. Aktiv schließen, damit accept() mit EBADF/EINVAL
    // zurückkehrt und der Thread die stop-Prüfung erreicht.
    if (server_fd >= 0) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    return NULL;
}

// Schreibt eine Antwort sowohl auf die Server-Konsole als auch (falls
// moeglich) direkt an den nc-Client zurueck, damit ein Bediener per
// "echo ... | nc" sofort sieht, ob/wie ein Befehl verarbeitet wurde -
// statt wie zuvor bei jedem Parse-Fehler komplett stillschweigend nichts
// zu tun.
static void ctrl_respond(int client, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("[CTRL] %s\n", msg);
    fflush(stdout);

    // Zeilenumbruch anhaengen, damit z.B. "nc" die Antwort sauber anzeigt.
    char out[300];
    int len = snprintf(out, sizeof(out), "%s\n", msg);
    if (len > 0) {
        ssize_t off = 0;
        while (off < len) {
            ssize_t w = write(client, out + off, len - off);
            if (w <= 0) break; // Client hat evtl. schon vor dem Lesen der Antwort getrennt - kein Beinbruch
            off += w;
        }
    }
}

// Setzt Wunsch-Rate/-Breite und wartet MIT HARTEM TIMEOUT darauf, dass
// smi_thread sie anwendet (siehe Kommentar bei settings_mutex weiter oben).
// Blockiert also NIE unbegrenzt, im Gegensatz zum fruehreren direkten
// ioctl()-Aufruf aus control_thread heraus.
static int wait_for_settings_applied(float rate, int width, double *out_real_msps) {
    pthread_mutex_lock(&settings_mutex);
    pending_rate = rate;
    pending_width = width;
    settings_dirty = 1;
    int seq_before = settings_apply_seq;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3; // grosszuegig: deckt auch niedrige MSPS-Raten mit langer Pufferdauer ab

    while (settings_apply_seq == seq_before && !stop) {
        int rc = pthread_cond_timedwait(&settings_applied_cond, &settings_mutex, &ts);
        if (rc == ETIMEDOUT) break;
    }
    int applied = (settings_apply_seq != seq_before);
    *out_real_msps = last_real_msps;
    pthread_mutex_unlock(&settings_mutex);
    return applied;
}

// --- Control-Thread: Port 5000 (Klartext, kompatibel + "dest" erweitert) ---
void *control_thread(void *arg) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[CTRL] socket"); return NULL; }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(CTRL_PORT) };
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CTRL] bind");
        close(server_fd);
        server_fd = -1;
        return NULL;
    }
    listen(server_fd, 5);

    float cur_rate = 5.0f;
    int cur_width = 16;

    while (!stop) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (stop) break;       // durch signal_thread aktiv geschlossen
            if (errno == EINTR) continue;
            perror("[CTRL] accept");
            break;
        }

        // TCP_NODELAY, damit unsere (kurze) Antwort ohne Nagle-Verzoegerung
        // sofort beim Client ankommt, bevor wir gleich wieder schliessen.
        int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char cmd[128] = {0};
        ssize_t n = read(client, cmd, sizeof(cmd) - 1);

        if (n <= 0) {
            printf("[CTRL] Verbindung ohne Daten empfangen (n=%zd)\n", n);
            fflush(stdout);
            close(client);
            continue;
        }

        cmd[n] = '\0';
        // Zeilenumbrueche/Whitespace am Ende entfernen (z.B. bei "echo" statt "echo -n")
        while (n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r' || cmd[n-1] == ' ')) {
            cmd[--n] = '\0';
        }

        if (strncmp(cmd, "rate ", 5) == 0) {
            float val = atof(cmd + 5);
            if (val > 0.1f && val <= 40.0f) {
                double real_msps;
                int applied = wait_for_settings_applied(val, cur_width, &real_msps);
                cur_rate = val;
                if (applied) {
                    ctrl_respond(client, "OK rate=%.3f MSPS (real %.4f) width=%d", cur_rate, real_msps, cur_width);
                } else {
                    ctrl_respond(client, "WARN rate=%.3f MSPS width=%d angefordert, Uebernahme dauert laenger als das Timeout (greift spaetestens beim naechsten Pufferzyklus)", cur_rate, cur_width);
                }
            } else {
                ctrl_respond(client, "ERR rate '%s' ausserhalb 0.1..40.0 MSPS", cmd + 5);
            }
        } else if (strncmp(cmd, "width ", 6) == 0) {
            int w = atoi(cmd + 6);
            if (w == 8 || w == 16) {
                double real_msps;
                int applied = wait_for_settings_applied(cur_rate, w, &real_msps);
                cur_width = w;
                if (applied) {
                    ctrl_respond(client, "OK rate=%.3f MSPS (real %.4f) width=%d", cur_rate, real_msps, cur_width);
                } else {
                    ctrl_respond(client, "WARN rate=%.3f MSPS width=%d angefordert, Uebernahme dauert laenger als das Timeout (greift spaetestens beim naechsten Pufferzyklus)", cur_rate, cur_width);
                }
            } else {
                ctrl_respond(client, "ERR width '%s' muss 8 oder 16 sein", cmd + 6);
            }

        } else if (strncmp(cmd, "dest ", 5) == 0) {
            char ip[64] = {0};
            int port = 0;
            int matched = sscanf(cmd + 5, "%63s %d", ip, &port);

            if (matched != 2) {
                ctrl_respond(client, "ERR 'dest' braucht IP UND Port, z.B. 'dest 192.168.1.135 1233' (erhalten: '%s')", cmd + 5);
            } else if (port <= 0 || port > 65535) {
                ctrl_respond(client, "ERR Port %d ausserhalb 1..65535", port);
            } else {
                struct sockaddr_in new_dest = {0};
                new_dest.sin_family = AF_INET;
                new_dest.sin_port = htons(port);
                if (inet_pton(AF_INET, ip, &new_dest.sin_addr) == 1) {
                    pthread_mutex_lock(&dest_mutex);
                    dest_addr = new_dest;
                    dest_configured = 1;
                    pthread_mutex_unlock(&dest_mutex);
                    ctrl_respond(client, "OK UDP-Ziel gesetzt: %s:%d", ip, port);
                } else {
                    ctrl_respond(client, "ERR ungueltige IP-Adresse: '%s'", ip);
                }
            }

        } else if (strncmp(cmd, "recalib", 7) == 0) {
            // Gezielte Neumessung der Core-Frequenz. Da control_thread ab
            // jetzt NICHT mehr mit Echtzeitprioritaet laeuft (siehe main()),
            // ist der popen()-Fork hier unkritisch. Die eigentliche
            // ioctl()-Anwendung passiert wie bei rate/width exklusiv in
            // smi_thread ueber denselben Handoff-Mechanismus.
            long new_freq = get_core_freq();
            g_core_freq = new_freq;
            double real_msps;
            int applied = wait_for_settings_applied(cur_rate, cur_width, &real_msps);
            if (applied) {
                ctrl_respond(client, "OK core_freq neu gemessen: %ld Hz, real %.4f MSPS uebernommen", new_freq, real_msps);
            } else {
                ctrl_respond(client, "WARN core_freq neu gemessen: %ld Hz, Uebernahme dauert laenger als das Timeout", new_freq);
            }

        } else {
            ctrl_respond(client, "ERR unbekannter Befehl: '%s' (erwartet: rate/width/dest/recalib)", cmd);
        }

        close(client);
    }

    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
    return NULL;
}

// --- SMI-Akquise-Thread: liest kontinuierlich vom ADC ----------------------
// Das ist der zeitkritische Pfad (Echtzeitprioritaet, siehe main()). Er darf
// NIEMALS unbegrenzt auf das Netzwerk warten - die Hardware liefert
// kontinuierlich Samples, egal ob ein Empfaenger mithaelt oder nicht.
void *smi_thread(void *arg) {
    int fill_idx = 0;

    while (!stop) {
        // Ausstehende rate/width/recalib-Aenderung anwenden, falls
        // control_thread eine hinterlegt hat. Geschieht hier bewusst EXKLUSIV
        // in diesem Thread, damit read() und ioctl() auf smi_fd nie von
        // verschiedenen Threads konkurrierend angefragt werden (das war die
        // Ursache des Priority-Inversion-Hangs, siehe Kommentar bei
        // settings_mutex weiter oben). Diese Pruefung ist billig (ein
        // Mutex-Lock) und laeuft einmal pro Pufferzyklus - bounded Latenz
        // fuer die Anwendung neuer Settings statt frueherem unbegrenztem
        // Haenger im control_thread.
        pthread_mutex_lock(&settings_mutex);
        if (settings_dirty) {
            float rate = pending_rate;
            int width = pending_width;
            settings_dirty = 0;
            pthread_mutex_unlock(&settings_mutex);

            update_smi_settings(rate, width);

            pthread_mutex_lock(&settings_mutex);
            settings_apply_seq++;
            pthread_cond_broadcast(&settings_applied_cond);
        }
        pthread_mutex_unlock(&settings_mutex);

        pthread_mutex_lock(&mutex);

        if (buf_busy[fill_idx]) {
            // Steht der Slot noch unversendet in der Queue? Dann ist er
            // (durch die strikte FIFO-Rotation von Akquise UND Versand)
            // garantiert der aelteste Eintrag -> wir duerfen ihn verwerfen,
            // um die Akquise nicht zu blockieren.
            if (queue_count > 0 && ready_queue[queue_head] == fill_idx) {
                queue_head = (queue_head + 1) % NUM_BUFFERS;
                queue_count--;
                buf_busy[fill_idx] = 0;
                stat_buffers_dropped++;
            } else {
                // Slot wird gerade aktiv per UDP versendet (nicht mehr in
                // der Queue) - hier duerfen wir NICHT hineinschreiben,
                // sonst Race mit dem laufenden sendto(). Kurz warten;
                // durch die Ringgroesse von 4 ist das auf die Sendezeit
                // eines einzelnen Puffers begrenzt.
                while (buf_busy[fill_idx] && !stop) {
                    pthread_cond_wait(&cond_free, &mutex);
                }
            }
        }

        if (stop) { pthread_mutex_unlock(&mutex); break; }
        buf_busy[fill_idx] = 1;
        pthread_mutex_unlock(&mutex);

        // --- Kritischer Abschnitt: DMA-Read vom ADC -------------------
        pthread_mutex_lock(&smi_io_mutex);
        uint8_t *ptr = buffers[fill_idx];
        size_t rx = 0;
        int read_error = 0;
        while (rx < BUFFER_SIZE) {
            ssize_t n = read(smi_fd, ptr + rx, BUFFER_SIZE - rx);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("[SMI] read");
                read_error = 1;
                break;
            } else if (n == 0) {
                read_error = 1; // unerwartetes EOF
                break;
            }
            rx += n;
        }
        pthread_mutex_unlock(&smi_io_mutex);

        pthread_mutex_lock(&mutex);
        if (!read_error && rx == BUFFER_SIZE) {
            queue_push_locked(fill_idx);
            stat_buffers_acquired++;
        } else {
            // Unvollstaendiger Puffer -> verwerfen statt fehlerhafte
            // Sample-Grenzen an GNU Radio zu schicken.
            buf_busy[fill_idx] = 0;
            pthread_cond_signal(&cond_free);
        }
        pthread_mutex_unlock(&mutex);

        fill_idx = (fill_idx + 1) % NUM_BUFFERS;
    }
    return NULL;
}

// Zerlegt einen Puffer in MTU-sichere UDP-Fragmente und sendet die rohen
// Bytes 1:1 ohne jegliches Zusatz-Framing - GNU Radios "UDP Source" liest
// diesen Strom direkt als Sample-Daten.
static void send_buffer_fragmented(int sock, uint8_t *data, size_t len, struct sockaddr_in *dest) {
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = (len - offset > UDP_PAYLOAD) ? UDP_PAYLOAD : (len - offset);

        ssize_t sent = sendto(sock, data + offset, chunk, 0,
                               (struct sockaddr *)dest, sizeof(*dest));
        if (sent < 0) {
            // EAGAIN/ENOBUFS bei vollem Sende-Puffer: Fragment verwerfen
            // statt zu blockieren (UDP - kein Backpressure gewuenscht).
            // Ohne Framing ist ein solcher Verlust auf dieser Ebene nicht
            // erkennbar (siehe Kommentar bei UDP_PAYLOAD weiter oben).
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
                perror("[NET] sendto");
            }
        }
        offset += chunk;
    }
}

// --- UDP-Sende-Thread -------------------------------------------------------
void *net_thread(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("[NET] socket"); return NULL; }

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    while (!stop) {
        pthread_mutex_lock(&mutex);
        while (queue_count == 0 && !stop) pthread_cond_wait(&cond_ready, &mutex);
        if (stop && queue_count == 0) { pthread_mutex_unlock(&mutex); break; }

        int idx = ready_queue[queue_head];
        queue_head = (queue_head + 1) % NUM_BUFFERS;
        queue_count--;
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&dest_mutex);
        int have_dest = dest_configured;
        struct sockaddr_in dest = dest_addr;
        pthread_mutex_unlock(&dest_mutex);

        if (have_dest) {
            send_buffer_fragmented(sock, buffers[idx], BUFFER_SIZE, &dest);
            stat_buffers_sent++;
        }
        // Kein Ziel konfiguriert: Puffer wird stillschweigend verworfen -
        // Akquise laeuft trotzdem unbeeinflusst weiter.

        pthread_mutex_lock(&mutex);
        buf_busy[idx] = 0;
        pthread_cond_signal(&cond_free);
        pthread_mutex_unlock(&mutex);
    }

    close(sock);
    return NULL;
}

// --- Statistik-Thread (optional, alle 5s eine Zeile Diagnose) --------------
void *stats_thread(void *arg) {
    while (!stop) {
        for (int i = 0; i < 50 && !stop; i++) usleep(100000); // 5s, aber reagiert zuegig auf stop
        if (stop) break;
        printf("[STATS] erfasst=%lu gesendet=%lu verworfen=%lu dest=%s\n",
               (unsigned long)stat_buffers_acquired,
               (unsigned long)stat_buffers_sent,
               (unsigned long)stat_buffers_dropped,
               dest_configured ? "gesetzt" : "NICHT gesetzt");
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    // Optionale Startargumente: initiales UDP-Ziel, damit man nicht
    // zwingend erst per Control-Port "dest" senden muss.
    if (argc == 3) {
        struct sockaddr_in initial = {0};
        initial.sin_family = AF_INET;
        initial.sin_port = htons(atoi(argv[2]));
        if (inet_pton(AF_INET, argv[1], &initial.sin_addr) == 1) {
            dest_addr = initial;
            dest_configured = 1;
            printf("[MAIN] Initiales UDP-Ziel: %s:%s\n", argv[1], argv[2]);
        } else {
            fprintf(stderr, "[MAIN] Ungueltige IP in Argument 1, ignoriere.\n");
        }
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [<ziel-ip> <ziel-port>]\n"
                        "  (Ziel kann auch spaeter per Control-Port 5000 mit 'dest <ip> <port>' gesetzt werden)\n",
                argv[0]);
        return 1;
    }

    // SIGINT/SIGTERM in diesem (und damit allen weiteren, davon
    // abgeleiteten) Threads blockieren, BEVOR irgendein Thread erzeugt
    // wird. Der signal_thread ist der einzige, der sie per sigwait()
    // synchron entgegennimmt.
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0) {
        perror("[MAIN] pthread_sigmask");
        return 1;
    }

    smi_fd = open("/dev/smi", O_RDWR);
    if (smi_fd < 0) {
        perror("[MAIN] open(/dev/smi)");
        fprintf(stderr, "[MAIN] Ist das 'smi-dev' Overlay geladen und laeuft das Programm mit ausreichenden Rechten?\n");
        return 1;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = malloc(BUFFER_SIZE);
        if (!buffers[i]) {
            fprintf(stderr, "[MAIN] Speicherzuweisung fuer Puffer %d fehlgeschlagen!\n", i);
            for (int j = 0; j < i; j++) free(buffers[j]);
            close(smi_fd);
            return 1;
        }
    }

    // Core-Frequenz EINMALIG hier messen, waehrend der Prozess noch ganz
    // normale Prioritaet hat (kein Thread traegt bereits SCHED_FIFO). Der
    // vcgencmd-Fork ist damit unkritisch. Alle spaeteren rate/width-Befehle
    // nutzen nur noch diesen gecachten Wert (siehe update_smi_settings).
    g_core_freq = get_core_freq();
    printf("[MAIN] Core-Frequenz einmalig gemessen: %ld Hz\n", g_core_freq);

    update_smi_settings(5, 16); // Startup-Default: 5 MSPS / 16 Bit

    pthread_t t_signal, t_smi, t_net, t_ctrl, t_stats;
    pthread_create(&t_signal, NULL, signal_thread, &sigset);
    pthread_create(&t_smi,    NULL, smi_thread,    NULL);
    pthread_create(&t_net,    NULL, net_thread,    NULL);
    pthread_create(&t_ctrl,   NULL, control_thread, NULL);
    pthread_create(&t_stats,  NULL, stats_thread,  NULL);

    // Echtzeitprioritaet NUR fuer den SMI-Akquise-Thread setzen - nicht
    // mehr prozessweit (frueher: sched_setscheduler(0, ...) VOR
    // pthread_create, wodurch ALLE Threads inkl. control_thread per
    // PTHREAD_INHERIT_SCHED auf SCHED_FIFO 99 liefen). Genau das hatte
    // den Bug verursacht: forkt control_thread via popen()/vcgencmd einen
    // Shell-Kindprozess, konkurriert dieser um die CPU mit dem daueraktiven
    // Echtzeit-Akquise-Thread und kann im schlimmsten Fall nie geschedult
    // werden - der Fork haengt dann auf unbestimmte Zeit, inklusive
    // gehaltenem smi_io_mutex, was den GESAMTEN Control-Kanal dauerhaft
    // blockiert (exakt das beobachtete Verhalten: Antwort kam erst beim
    // Shutdown). Jetzt laufen control_thread/net_thread/stats_thread mit
    // normaler Prioritaet und koennen einander sowie kurzlebige Kindprozesse
    // nicht mehr blockieren; nur die zeitkritische ADC-Akquise bekommt
    // echte Echtzeitgarantien.
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(t_smi, SCHED_FIFO, &sp) != 0) {
        perror("[MAIN] Warnung: Konnte Real-Time Prioritaet fuer smi_thread nicht setzen (sudo vergessen?)");
    }

    printf("[MAIN] smisdr_rx laeuft. Steuerkanal: Port %d (rate/width/dest/recalib). UDP-Datenziel-Port beim Empfaenger: %d\n",
           CTRL_PORT, DATA_PORT_RX);
    printf("[MAIN] Beenden mit Strg+C (sauberer Shutdown ueber sigwait).\n");

    pthread_join(t_signal, NULL);
    pthread_join(t_smi, NULL);
    pthread_join(t_net, NULL);
    pthread_join(t_ctrl, NULL);
    pthread_join(t_stats, NULL);

    printf("[MAIN] Shutdown abgeschlossen. Statistik: erfasst=%lu gesendet=%lu verworfen=%lu\n",
           (unsigned long)stat_buffers_acquired,
           (unsigned long)stat_buffers_sent,
           (unsigned long)stat_buffers_dropped);

    if (smi_fd >= 0) {
        if (close(smi_fd) == -1) perror("[CLEANUP] close(smi_fd)");
        smi_fd = -1;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(buffers[i]);
        buffers[i] = NULL;
    }

    pthread_mutex_destroy(&mutex);
    pthread_mutex_destroy(&smi_io_mutex);
    pthread_mutex_destroy(&dest_mutex);
    pthread_mutex_destroy(&settings_mutex);
    pthread_cond_destroy(&cond_free);
    pthread_cond_destroy(&cond_ready);
    pthread_cond_destroy(&settings_applied_cond);

    return 0;
}
