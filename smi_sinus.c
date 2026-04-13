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
 * Die GPIOs müssen ggf. auf die Alternate Function 'SMI' gesetzt werden.
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



#include <linux/broadcom/bcm2835_smi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <signal.h>


// system-spezifischen Fixes für Kernel 6.12 (32-Bit)
#undef BCM2835_SMI_IOC_MAGIC
#undef BCM2835_SMI_IOC_WRITE_SETTINGS
#define BCM2835_SMI_IOC_MAGIC 0x01
#define BCM2835_SMI_IOC_WRITE_SETTINGS _IO(BCM2835_SMI_IOC_MAGIC, 1)

#define BUFFER_SIZE (4 * 1024 * 1024) // 4 MB
#define SAMPLE_RATE 5000000.0         // 5 MSPS
#define SINE_FREQ   1000000.0         // 1 MHz

volatile sig_atomic_t stop = 0;
int sig_count = 0;

int fd;
struct smi_settings settings;

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
void update_smi_settings(int msps, int width) {
    struct smi_settings settings;
    //memset(&settings, 0, sizeof(settings));

    long core_f = get_core_freq();
    printf("core_f=%ld\n",core_f);

    // Faktor 2 Korrektur für Pi 4 / 16-Bit Modus
    int smi_divisor = 2;
    int total_cycles = core_f / (msps * 1000000 * smi_divisor);

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

    if (ioctl(fd, BCM2835_SMI_IOC_WRITE_SETTINGS, &settings) == 0) {
        // Ausgabe der echten Rate zur Kontrolle am Terminal
        double real_msps = (double)core_f / (total_cycles * smi_divisor * 1000000.0);
        printf("[CTRL] Update: Ziel %d MSPS -> Real %.4f MSPS (Cycles: %d [%d/%d/%d])\n",
               msps, real_msps, total_cycles, setup, strobe, hold);
    }
}


void handle_sigint(int sig) {
    sig_count++;

    if (sig_count == 1) {
      stop = 1;  // normal termination / cleanup
    } else {  
      _exit(1);  // hard termination
    }
}

int main() {
    struct sigaction sa;
    sa.sa_handler = &handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // 1. Device öffnen
    fd = open("/dev/smi", O_RDWR);
    if (fd < 0) {
        perror("Fehler beim Öffnen von /dev/smi");
        return 1;
    }

    update_smi_settings(5, 16); // Start-Default 5 MSPS / 16 Bit

    // 3. 4MB Puffer mit 1MHz Sinus füllen
    uint16_t *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("Speicherfehler");
        return 1;
    }

    printf("Generiere 1 MHz Sinus bei 5 MSPS...\n");
    for (size_t i = 0; i < BUFFER_SIZE / 2; i++) {
        // 5 Samples pro Periode (5MSPS / 1MHz = 5)
        //buffer[i] = (uint16_t)(32767.0 * sin(2.0 * M_PI * SINE_FREQ * i / SAMPLE_RATE) + 32768);
       buffer[i] = (uint16_t)(128.0 * sin(2.0 * M_PI * SINE_FREQ * i / SAMPLE_RATE) + 128);
   }


    // 4. Endlose Ausgabe
    printf("Starte kontinuierliche Übertragung. Beenden mit Strg+C.\n");
    while (!stop) {
        ssize_t written = write(fd, buffer, BUFFER_SIZE);
        if (written < 0) {
            if (stop) break;
            perror("Fehler beim Schreiben auf SMI");
            break;
        }
    }

    free(buffer);
    close(fd);
    return 0;
}
