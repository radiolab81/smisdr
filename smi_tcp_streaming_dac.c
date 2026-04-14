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
 * SD6  : GPIO 14 (Pin  8) | SD14 : GPIO 24 (Pin 15)
 * SD7  : GPIO 15 (Pin 10) | SD15 : GPIO 25 (Pin 26)
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

uint8_t *buffer_a, *buffer_b, *active_buffer;
int buffer_ready = 0, smi_fd;
int buf_a_busy = 0, buf_b_busy = 0;

volatile sig_atomic_t stop = 0;
int sig_count = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_free = PTHREAD_COND_INITIALIZER; // Signal für "Puffer frei"
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


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
    struct smi_settings settings;

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
            cur_rate = atof(cmd + 5);
            update_smi_settings(cur_rate, cur_width);
        } else if (strncmp(cmd, "width ", 6) == 0) {
            cur_width = atoi(cmd + 6);
            update_smi_settings(cur_rate, cur_width);
        }
        close(client);
    }
    return NULL;
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

        uint8_t *fill_ptr = buffer_a;
        while (!stop) {

            // 1. WARTEN: Ist der Puffer, den ich füllen will, noch beim SMI-Treiber?
            pthread_mutex_lock(&mutex);
            while ((fill_ptr == buffer_a && buf_a_busy) || (fill_ptr == buffer_b && buf_b_busy)) {
                pthread_cond_wait(&cond_free, &mutex);
            }
            pthread_mutex_unlock(&mutex);

            // 2. FÜLLEN: Daten vom Netzwerk lesen
            size_t rx = 0;
            while (rx < BUFFER_SIZE) {
                ssize_t n = read(client_fd, fill_ptr + rx, BUFFER_SIZE - rx);
                if (n <= 0) {
                    printf("[DATA] Client getrennt.\n");
                    goto disconnect;
                }
                rx += n;
            }

            // 3. SIGNAL: Puffer voll, dem Main-Thread bescheid geben
            pthread_mutex_lock(&mutex);
            active_buffer = fill_ptr;

            if (fill_ptr == buffer_a) buf_a_busy = 1; else buf_b_busy = 1;
            buffer_ready = 1;
            pthread_cond_signal(&cond);

            // Puffer für den nächsten Durchlauf wechseln
            fill_ptr = (fill_ptr == buffer_a) ? buffer_b : buffer_a;
            pthread_mutex_unlock(&mutex);
        }
        disconnect: close(client_fd);
        // Nach Disconnect: Beide Puffer als frei markieren für neuen Versuch
        pthread_mutex_lock(&mutex);
        buf_a_busy = 0;
        buf_b_busy = 0;
        buffer_ready = 0;
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

    update_smi_settings(5, 16); // startup default 5 MSPS / 16 bit mode

    pthread_t net_t, ctrl_t;
    pthread_create(&net_t, NULL, network_thread, NULL);
    pthread_create(&ctrl_t, NULL, control_thread, NULL);

    while (!stop) {
        pthread_mutex_lock(&mutex);
        while (!buffer_ready) pthread_cond_wait(&cond, &mutex);
        uint8_t *data = active_buffer; buffer_ready = 0;
        pthread_mutex_unlock(&mutex);

        write(smi_fd, data, BUFFER_SIZE);

        pthread_mutex_lock(&mutex);
        if (data == buffer_a) buf_a_busy = 0; else buf_b_busy = 0;
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
