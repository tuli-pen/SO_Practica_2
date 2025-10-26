/* p2-search.c
 *
 * Search worker que se comunica con la UI mediante sockets
 * Búsqueda por SUBCADENA (case-insensitive) en title + filtro opcional update_date (col 12).
 *
 * Requisitos:
 *  - index.h (IndexHeader, BucketDisk, EntryDisk, KEY_SIZE, N_BUCKETS)
 *  - hash.h / hash.c (hash_string)
 *  - build_index(...) en index2.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> 
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

#include "common.h"    /* FIFO_REQ, FIFO_RES, Request, Response */
#include "index.h"
#include "hash.h"

#ifndef KEY_SIZE // ifndef significa “if not defined”
#define KEY_SIZE 256 // max tamaño de título
#endif

#define CSV_FILE "arxiv.csv"
#define INDEX_FILE "index.bin"
#define MAX_LINE 8192
#define MAX_RESULTS 50
#define BUCKET_RANGE 12    // Heurística: escanear 12 vecinos alrededor del bucket

#define PORT 12345 // puerto que usaremos
#define SEM_NAME "/socket_sync_sem" // nombre del semáforo
#define BACKLOG 10 // longitud máxima de la cola de conexiones pendientes

static int listen_fd = -1; // descriptor de archivo de socket que escucha
static sem_t *sem = NULL; // inicializamos puntero al semáforo

int build_index(const char *csv_path, const char *index_path);

// ============================================================================

// Guarda un nuevo registro en el CSV y reindexa después
int save_new_register(const char *str) {

    printf("Guardando %s...\n", str);

    // ############## AQUÍ VA EL CÓDIGO DE NICO #####################

    // Debe crear un nuevo registro con el titulo dado y luego reindexar

    // Debe devolver 0 si guarda el nuevo registro exitosamente y -1 si tiene un error

    return 0;
}

// ============================================================================

// Función para cerrar recursos cuando ocurra SIGINT (CNTL+C)
void handle_sigint(int sig) {
    (void)sig; // no usaremos sig
    printf("\nServidor: SIGINT recibido, cerrando...\n");
    if (listen_fd >= 0) close(listen_fd);
    if (sem != SEM_FAILED && sem != NULL) {
        sem_close(sem);
        sem_unlink(SEM_NAME);
    }
    exit(0);
}

// ============================================================================

// Lee n bytes de fd y los pone en buf
// Devuelve la cantidad de bytes leídos, o -1 en error
// Lee hasta terminar o hasta que ocurra un error no recuperable
ssize_t readn(int fd, void *buf, size_t n) {

    size_t left = n; // left lleva la cuenta de cuántos bytes faltan por leer
    char *p = buf; // el puntero p apunta al inicio del buffer buf

    while (left > 0) {

        // Lee left bytes de fd y los pone en p (buf)
        ssize_t r = read(fd, p, left);
        if (r < 0) {

            // Error por interrupción de señal, volver a intentar
            if (errno == EINTR) continue;

            // Error de otro tipo
            return -1;
        }
        if (r == 0) break; // Si r=0 entonces read devolvió EOF 
        left -= r;
        p += r;
    }
    return (n - left);
}

// ============================================================================

// Escribe n bytes de buf en fd
// Devuelve la cantidad de bytes escritos, o -1 en error
// Escribe hasta terminar o hasta que ocurra un error no recuperable
ssize_t writen(int fd, const void *buf, size_t n) {

    size_t left = n; // left lleva la cuenta de cuántos bytes faltan por escribir
    const char *p = buf; // el puntero p apunta al inicio del buffer buf

    while (left > 0) {

        // Escribe left bytes de p (buf) en fd
        ssize_t w = write(fd, p, left);

        if (w <= 0) {

            // Error por interrupción de señal, volver a intentar
            if (w < 0 && errno == EINTR) continue;

            // Error de otro tipo
            return -1;
        }
        left -= w;
        p += w;
    }
    return n;
}

// ============================================================================

// Busca una subcadena (needle) dentro de otra cadena (haystack),
// sin distinguir entre mayúsculas y minúsculas (case-insensitive)
static char *ci_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char *)haystack;
    for (const char *p = haystack; *p; ++p) {

        // si no hay suficiente espacio de ahí en adelante,
        // de una dice que no se encontró la subcadena
        size_t rem = strlen(p);
        if (rem < needle_len) return NULL;

        // strncasecmp compara las dos cadenas, devuelve 0 si son iguales
        if (strncasecmp(p, needle, needle_len) == 0) return (char *)p; 
    }
    return NULL;
}

// ============================================================================

// Elimina espacios en blanco al inicio y al final de una cadena,
// modificando directamente el mismo string (in-place)
static void trim_inplace(char *s) {
    if (!s) return;
    char *a = s;

    // Avanza a mientras haya espacios al inicio
    while (*a && isspace((unsigned char)*a)) a++;

    if (a != s) memmove(s, a, strlen(a)+1); // Copiar a en s
    size_t n = strlen(s);

    // Mientras haya espacios al final pone un fin de cadena
    while (n > 0 && isspace((unsigned char)s[n-1])) {
        s[n-1] = '\0';
        n--;
    }
}

// ============================================================================

// Extrae el contenido de una columna específica (target_col) de una línea CSV.
// Devuelve 1 si logró obtener la columna, 0 si no.
static int csv_get_column(const char *line, int target_col, char *out, size_t out_sz) {
    int col = 1;
    const char *p = line;

    // Itera hasta final de cadena o hasta una nueva línea
    while (*p && *p != '\n' && *p != '\r') {

        // Si estamos en la columna deseada...
        if (col == target_col) {
            
            // Campo con comillas: 
            if (*p == '"') {
                p++; // Saltarse la comilla de apertura
                size_t pos = 0; // pos es el índice para escribir en out

                while (*p) { 
                    if (*p == '"') {    // Si hay una comilla

                        if (*(p+1) == '"') {  // Si hay dos comillas (Hay una 
                                              // comilla escapada en el CSV)

                            // Escribe una comilla en out[pos] e incrementa pos
                            if (pos + 1 < out_sz) out[pos++] = '"'; 
                            p += 2;
                            continue;
                        }

                        // Una sola comilla indica el final del campo en el csv
                        else { p++; break; } 
                    }

                    // Si el caracter actual no es una comilla, lo guardamos en out[pos]
                    if (pos + 1 < out_sz) out[pos++] = *p;
                    p++;
                }
                out[pos] = '\0'; // Cierra la cadena out
                trim_inplace(out); // Quita espacios iniciales y finales en out
                return 1;
            }
            
            // Campo sin comillas: copiar hasta la próxima coma o fin de línea
            else {
                const char *start = p;

                // Bucle que avanza p hasta el final del campo
                while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;

                size_t len = (size_t)(p - start);
                if (len >= out_sz) len = out_sz - 1;

                // Copia el contenido del campo en out
                memcpy(out, start, len);

                out[len] = '\0'; // Cierra la cadena out
                trim_inplace(out); // Quita espacios iniciales y finales en out
                return 1;
            }
        }

        // No estamos en la columna deseada...
        // Queremos saltarnos esa columna

        // Campo con comillas:
        if (*p == '"') {
            p++;

            while (*p) {
                if (*p == '"' && *(p+1) != '"') { // Si hay 1 sola comilla
                    p++;    // Avanza 1 caracter
                    break;  // Sale porque ya llegó al final del campo
                }

                if (*p == '"' && *(p+1) == '"') { // Si hay dos comillas (Hay una 
                    p += 2;                       // comilla escapada en el CSV)
                }   
                                              
                else p++; // Si no hay una comilla, avanza 1 caracter
            }

            // En CSV la coma es el separador de campos
            // Si estamos en la coma, ya nos saltamos el campo exitosamente
            if (*p == ',') {
                p++;
                col++;
            }
        } 

        // Campo sin comillas:
        else { 
            // Saltar el caracter si no es coma o salto de línea
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;

            // En CSV la coma es el separador de campos
            // Si estamos en la coma, ya nos saltamos el campo exitosamente
            if (*p == ',') {
                p++;
                col++;
            }
        }
    }
    out[0] = '\0';
    return 0; // Devuelve 0 si no encontró la columna deseada
}

// ============================================================================

// Compara dos cadenas, ignorando mayúsculas/minúsculas
// (case-insensitive) y espacios al inicio o al final
// Devuelve 1 si son iguales, 0 si son diferentes

// static int field_is(const char *field, const char *target) {

//     if (!field || !target) return 0; // Si alguno es NULL, devuelve 0

//     char a[128], b[128];

//     strncpy(a, field, sizeof(a)-1); // Copiar field en a
//     a[sizeof(a)-1] = '\0';
//     trim_inplace(a);

//     strncpy(b, target, sizeof(b)-1); // Copiar target en a
//     b[sizeof(b)-1] = '\0';
//     trim_inplace(b);

//     return strcasecmp(a, b) == 0; // strcasecmp devuelve 0 si son iguales
// }

// ============================================================================

// Busca en el CSV, en un rango de buckets vecinos [-12,+12]
// Usa coincidencia de subcadena para el título. Filtro exacto para la fecha.
// Guarda las líneas del CSV que coincidan en resp_buf (de tamaño resp_sz).
// Devuelve: el número de coincidencias encontradas (>0)
//           0 si no hay ninguna, o
//           -1 si ocurre un error
static int search_by_title_and_update(const char *title_value, const char *update_value,
                                      char *resp_buf, size_t resp_sz) {
    if (!title_value || resp_buf == NULL) return -1; // Devuelve -1 si alguno es NULL


    /* VERFICAR SI YA EXISTE EL ÍNDICE, SI NO ENTONCES CREARLO */

    // Verifica si se puede acceder a "index.bin"
    // access devuelve 0 si la comprobación de acceso es exitosa
    if (access(INDEX_FILE, F_OK) != 0) {  // F_OK solo pide comprobar la existencia del archivo

        // Si "index.bin" no existe, entonces lo crea con build_index
        if (build_index(CSV_FILE, INDEX_FILE) != 0) return -1;
    }

    /* ABRIR ARCHIVOS */

    // idx es un puntero a "index.bin" abierto en modo lectura binaria
    FILE *idx = fopen(INDEX_FILE, "rb"); 

    // csv es un puntero a "arxiv.csv" abierto en modo lectura
    FILE *csv = fopen(CSV_FILE, "r");

    // Verifica si alguno de los dos archivos no se pudo abrir
    // (Si fopen falla devuelve NULL)
    if (!idx || !csv) {

        // Cierra los dos archivos porque hubo un error
        if (idx) fclose(idx);
        if (csv) fclose(csv);
        return -1;
    }

    /* LEER HEADER */

    IndexHeader header; // Aquí guardaremos el header leído de "index.bin"

    // fseek mueve el puntero idx 0 bytes desde el inicio, devuelve 0 si lo logra
    // fread lee el index header de "index.bin" y lo guarda en header, devuelve el número de elementos leídos si lo logra 
    if (fseek(idx, 0, SEEK_SET) != 0 || fread(&header, sizeof(IndexHeader), 1, idx) != 1) {

        // Cierra los dos archivos porque hubo un error
        fclose(idx); fclose(csv);
        return -1;
    }

    /* CALCULAR HASH DEL TÍTULO QUE QUEREMOS */

    long n_buckets = header.n_buckets; // Trae el núm total de buckets en index.bin (1000)
    if (n_buckets <= 0) n_buckets = N_BUCKETS; // Verifica si es un bucket válido

    // h es el hash del título buscado, módulo 1000
    unsigned long h = hash_string(title_value) % (unsigned long)n_buckets;


    /* INICIALIZACIONES */

    int found = 0; // found será la cantidad de líneas del csv encontradas
    size_t used = 0; // used será la cantidad de bytes ya ocupados en resp_buf
    resp_buf[0] = '\0'; // en resp_buf se guardarán las líneas del csv encontradas

    int off_start = -BUCKET_RANGE; // Buscaremos desde -12
    int off_end   = BUCKET_RANGE;  // hasta +12

    char linebuf[MAX_LINE]; // Aquí se guardará una línea del csv 


    /* BUSCAAAAAARR */

    // Aquí iteramos por bucket
    // Vamos a buscar desde el bucket h - 12 hasta el bucket h + 12
    for (int off = off_start; off <= off_end && found < MAX_RESULTS; ++off) {


        /* CALCULAR OFFSET DEL BUCKET QUE QUEREMOS */

        // bucket_idx es el número de bucket del bucket que queremos
        long bucket_idx = (long)h + off; // hash calculado (h), más el offset
        if (bucket_idx < 0 || bucket_idx >= n_buckets) continue; // si está fuera del rago de 0 a 1000, no se procesa ese bucket

        // Calculamos el offset del buccket que queremos leer
        long bucket_offset = sizeof(IndexHeader) + sizeof(BucketDisk) * bucket_idx;


        /* LEEMOS EL BUCKET */

        BucketDisk b; // aquí guardaremos el bucket leído
        if (fseek(idx, bucket_offset, SEEK_SET) != 0) continue; // movemos idx al offset del bucket
        if (fread(&b, sizeof(BucketDisk), 1, idx) != 1) continue; // leemos el bucket y lo guardamos en b


        /* VAMOS A LEER LOS ENTRIEEES */

        // current empieza siendo el offset del primer entry que queremos leer:
        long current = b.first_entry_offset;

        EntryDisk entry; // aquí guardaremos el entry que vamos leyendo

        // current iterará para leer cada entry, current es el entry actual
        // cuando current = -1 es cuando hemos llegado al final de la lista enlazada
        while (current != -1 && found < MAX_RESULTS) {

            if (fseek(idx, current, SEEK_SET) != 0) break; // movemos idx al offset del entry actual
            if (fread(&entry, sizeof(EntryDisk), 1, idx) != 1) break; // leemos el entry y lo guardamos en entry

            // Busca el título que queremos (como subcadena) en el entry actual, case-insensitive
            // Entra en el if si hay coincidencia
            if (ci_strcasestr(entry.key, title_value)) {


                /* LEER EN EL CSV */

                // Mueve el puntero csv al offset que nos dice el entry, si tiene exito entra al if
                if (fseek(csv, entry.csv_offset, SEEK_SET) == 0) {

                    // Trae una línea del csv y la guarda en linebuf, si tiene éxito entra en el if
                    if (fgets(linebuf, sizeof(linebuf), csv)) {


                        /* FILTRO DE FECHA */

                        // pass_update indicará si pasa el filtro de fecha o no
                        int pass_update = 1;

                        // Entra a este if solo si existe un valor de update_value
                        if (update_value && update_value[0] != '\0') {

                            // aquí se guardará la fecha extraída del csv
                            char parsed_update[64];

                            // extrae la columna 12 de linebuf y la guarda en parsed_update
                            // solo entra el if si NO pudo extraer la columna
                            if (!csv_get_column(linebuf, 12, parsed_update, sizeof(parsed_update))) {
                                pass_update = 0; // no pasa el filtro de fecha
                            }
                            else {

                                // si pudo extraer la fecha, pero la fecha no coincide entonces
                                // no pasa el filtro de fecha
                                if (strcasecmp(parsed_update, update_value) != 0) pass_update = 0;

                            }
                        }

                        // Si pasó el filtro de fecha:
                        if (pass_update) {

                            // line_len es la longitud de linebuf
                            size_t line_len = strnlen(linebuf, sizeof(linebuf));

                            // Entra a este if si hay suficiente espacio en resp_buf
                            if (used + line_len + 1 < resp_sz) {

                                // Copia la línea del csv (linebuf) en resp_buf, desde el
                                // punto en el que llenó resp_buf la última vez
                                memcpy(resp_buf + used, linebuf, line_len);

                                used += line_len; // aumenta used (cantiad de bytes guardados en resp_buf)
                                resp_buf[used] = '\0'; // pone fin de cadena al final
                                found++; // aumenta el contador de líneas del csv encontradas
                            }
                            else {
                                // No queda espacio en resp_buf, termina la búsqueda
                                goto FINISH_SEARCH;
                            }
                        }
                    } 
                }
            }

            // Va al siguiente entry (el siguiente está enlazado 
            // al actual, es una lista enlazada)
            current = entry.next_entry;
        }

    }

// FINISH_SEARCH es una etiqueta de C, no ejecuta nada ella misma, solo
// marca una posición en el código a la que se puede saltar.
FINISH_SEARCH:

    // Cierra los dos archivos y devuelve la cantidad de líneas encontradas
    fclose(idx);
    fclose(csv);
    return found;
}

// ============================================================================


/* MAAAAAIN */

int main(void) {

    struct sockaddr_in addr; // declaramos una estructura que se usa para describir direcciones IPv4
    int client_fd; // descriptor del socket que hablará con un cliente en específico


    /* DEFINIR HANDLER PARA SIGINT (CNTRL+C)*/

    // Definimos sa (una estructura sigaction)
    struct sigaction sa; // sigaction es una estructura que describe cómo manejar una señal
    sa.sa_handler = handle_sigint; // establecemos la función handle_sigint como handler
    sigemptyset(&sa.sa_mask); // no debe bloquearse ninguna señal adicional mientras corre el handler
    sa.sa_flags = 0; // sin comportamientos especiales

    // Registramos lo definido en sa para la señal SIGINT
    sigaction(SIGINT, &sa, NULL);


    /* CREAR O ABRIR EL SEMÁFORO */

    // Crea un semáforo, solo si no existe
    // Si ya existe, la llamada falla con errno = EEXIST
    // Permisos 0600 significa rw sólo para el propietario
    sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0600, 0);

    if (sem == SEM_FAILED) {
        if (errno == EEXIST) {

            // Ya existe el semáforo, entonces lo abrimos
            sem = sem_open(SEM_NAME, 0);
            if (sem == SEM_FAILED) {
                perror("sem_open existente falló");
                exit(EXIT_FAILURE);
            }
        } else {
            perror("sem_open falló");
            exit(EXIT_FAILURE);
        }
    }

    /* CREAR SOCKET QUE ESCUCHA */

    // En listen_fd queda el descriptor de archivo del socket
    // domain=AF_INET significa que el socket usará IPv4
    // type=SOCK_STREAM indica que el tipo de socket es orientado a conexión
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Error al crear el socket
    if (listen_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }


    /* REUSAR EL PUERTO 12345 */

    int opt = 1;

    // setsockopt configura una opción en el socket listen_fd
    // SOL_SOCKET indica que es una opción de socket genérica
    // SO_REUSEADDR es el nombre de la opción que queremos cambiar (permite reusar un puerto)
    // le pasamos opt=1 porque queremos activar la opción
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));


    /* CONFIGURAR SOCKET QUE ESCUCHA */

    memset(&addr, 0, sizeof(addr)); // llena de 0s la estructura addr

    addr.sin_family = AF_INET; // la familia de direcciones será IPv4

    // hton (host to network) reordena los bytes a big-endian
    // la l o s es de long o small
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // como IP usamos local host (127.0.0.1)
    addr.sin_port = htons(PORT); // como puerto usamos 12345

    // BIND
    // bind() sirve para asociar un socket con una dirección (IP y puerto)
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {

        // Error en el bind
        perror("bind");
        close(listen_fd);
        sem_close(sem);
        sem_unlink(SEM_NAME);
        exit(EXIT_FAILURE);
    }


    /* LISTEN */

    // listen() pone el socket en modo esperando conexiones entrantes
    // BACKLOG=10 es el tamaño máximo de la cola de espera de conexiones
    if (listen(listen_fd, BACKLOG) < 0) {

        // Error en el listen
        perror("listen");
        close(listen_fd);
        sem_close(sem);
        sem_unlink(SEM_NAME);
        exit(EXIT_FAILURE);
    }

    printf("Servidor: escuchando solicitudes en 127.0.0.1:%d\n", PORT);


    /* BUCLE INFINITO: aceptar peticiones y procesarlas */

    for (;;) {  

        /* SEMÁFORO VERDE */

        // Señalizamos al cliente (post) para que sepa que el servidor está listo
        if (sem_post(sem) < 0) perror("sem_post");


        /* ACEPTAR CONEXIÓN */

        // En esta línea creamos el socket client_fd, que hablará con el cliente,
        // ese nuevo socket es exclusivo para esa conexión con ese cliente
        client_fd = accept(listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) continue; // error por interrupción
            perror("accept"); // otro error
            break;
        }


        /* LEER PETICIÓN */

        // Protocolo en el que envía el cliente:
        // - 4 bytes para el comando (1 o 2)
        // - 4 bytes para la longitud del string que viene a continuación
        // - n bytes para el string

        // uint32_t es un entero sin signo de 32 bits (4 bytes)
        uint32_t cmd_net, len_net; // estará en big-endian
        uint32_t cmd, len; // estará en little-endian

        // LEER comando
        if (readn(client_fd, &cmd_net, sizeof(cmd_net)) != sizeof(cmd_net)) {
            fprintf(stderr, "Servidor: fallo leyendo comando\n");
            close(client_fd);
            continue;
        }
        
        // LEER tamaño string
        if (readn(client_fd, &len_net, sizeof(len_net)) != sizeof(len_net)) {
            fprintf(stderr, "Servidor: fallo leyendo longitud\n");
            close(client_fd);
            continue;
        }

        // ntohl es network to host long
        // aquí convertimos a little-endian
        cmd = ntohl(cmd_net);
        len = ntohl(len_net);

        // Reserva (en el heap) len + 1 bytes de memoria
        char *buf = malloc(len + 1); 

        if (!buf) {

            // Error en el malloc
            perror("malloc");
            close(client_fd);
            continue;
        }

        // LEER string
        if (readn(client_fd, buf, len) != (ssize_t)len) {
            fprintf(stderr, "Servidor: fallo leyendo string\n");
            free(buf);
            close(client_fd);
            continue;
        }
        buf[len] = '\0';


        /* OPCIÓN 1: REALIZAR BÚSQUEDA */

        if (cmd == 1) {
            
            printf("Buscando %s...\n", buf);

            char resp[8192];

            // BÚSQUEDA
            int found = search_by_title_and_update(buf, NULL, resp, sizeof(resp));


            /* ENVIAR RESPUESTA AL CLIENTE */

            // Protocolo en el que envía el servidor: 
            // - 4 bytes para el número de bytes del mensaje
            // - n bytes para el mensaje

            // Si no encontró resultados, envía NA
            if (found <= 0) {
                const char *na = "NA";
                uint32_t na_len_net = htonl((uint32_t)strlen(na));
                writen(client_fd, &na_len_net, sizeof(na_len_net)); // enviar tamaño mensaje
                writen(client_fd, na, strlen(na)); // enviar mensaje
            }

            // Si encontró resultados, los envía
            else {
                uint32_t resp_len_net = htonl((uint32_t)strlen(resp));
                writen(client_fd, &resp_len_net, sizeof(resp_len_net)); // enviar tamaño mensaje
                writen(client_fd, resp, strlen(resp)); // enviar mensaje
            }
        }
        

        /* OPCIÓN 2: GUARDAR NUEVO REGISTRO*/
    
        else if (cmd == 2) {

            // GUARDAR REGISTRO
            int saved = save_new_register(buf);

            /* ENVIAR RESPUESTA AL CLIENTE */

            // Protocolo en el que envía el servidor: 
            // - 4 bytes para el número de bytes del mensaje
            // - n bytes para el mensaje

            // Si logró guardar el registro
            if (saved == 0) {
                // Enviar un ACK (confirmación)
                const char *ack = "OK";
                uint32_t ack_len_net = htonl((uint32_t)strlen(ack));
                writen(client_fd, &ack_len_net, sizeof(ack_len_net)); // tamaño mensaje
                writen(client_fd, ack, strlen(ack)); // mensaje
            }

            // Si no se pudo guardar el registro
            else {
                const char *err = "ERROR: no se pudo guardar el registro";
                uint32_t err_len_net = htonl((uint32_t)strlen(err));
                writen(client_fd, &err_len_net, sizeof(err_len_net)); // enviar tamaño mensaje
                writen(client_fd, err, strlen(err)); // enviar mensaje
            }

        }
        
        /* COMANDO DESCONOCIDO */
        else {
            printf("Comando desconocido (%u) para '%s'\n", cmd, buf);
        }
      
        free(buf);
        close(client_fd);
    }

    return 0;
}
