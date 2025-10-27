
# 🧩 Práctica 2 — Sistemas Operativos

## 👥 Integrantes del Grupo
- **Misael Jesús Flórez Anave** — Cliente (`p2-dataProgram.c`)
- **Nicolay Prieto Mendoza** — Indexación y escritura (`index.c`, `hash.c`, `index.bin`)
- **Tuli Peña Melo** — Servidor (`p2-searchd.c`)

---

## 📘 Descripción General

Esta práctica implementa un sistema **Cliente-Servidor** que permite buscar, leer y escribir registros sobre un dataset de gran tamaño (basado en el dataset `arxiv.csv`, de papers académicos).  

El proyecto se divide en **tres componentes principales**, desarrollados de forma modular por cada integrante del grupo:

| Componente | Archivo principal | Responsable | Descripción |
|-------------|------------------|--------------|--------------|
| 🖥️ Servidor | `p2-searchd.c` | Tuli Peña Melo | Gestiona la comunicación principal, recibe solicitudes, accede al dataset y responde resultados. |
| 💻 Cliente | `p2-dataProgram.c` | Misael Flórez Anave | Proporciona el menú interactivo, envía comandos al servidor y muestra los resultados. |
| ⚙️ Indexador | `index.c`, `hash.c`, `index.bin` | Nicolay Prieto Mendoza | Gestiona la indexación, lectura y escritura eficiente de los registros en el dataset. |

---

## 🧠 Dataset Elegido
**`arxiv.csv`**

- Dataset de aproximadamente **1.7 millones** de papers científicos.  
- Tamaño real: **~3.9 GB**.  
- Para pruebas locales se utiliza una versión reducida (`data/dataset.csv`).

### 📊 Campos Principales
| Campo | Tipo | Descripción |
|--------|------|-------------|
| id | numérico (float) | Identificador único |
| submitter | string | Persona que sube el paper |
| authors | string | Lista de autores |
| title | string | Campo indexado (primario) |
| categories | string | Clasificación del paper |
| comments | string | Comentarios adicionales |
| license | string | Licencia |
| abstract | string | Resumen del artículo |
| versions | string | Historial de versiones |
| update_date | string | Fecha de actualización |

---

## 🧱 Estructura del Proyecto
```

practica2/
├─ src/
│  ├─ p2-searchd.c       # Servidor (Tuli)
│  ├─ p2-dataProgram.c   # Cliente (Misael)
│  ├─ common.h           # Definiciones compartidas
│  ├─ hash.c / hash.h    # Funciones hash (Nico)
│  ├─ index.c / index.h  # Indexador (Nico)
│  └─ ...
├─ data/
│  ├─ dataset.csv        # Versión reducida del dataset
│  └─ index.bin          # Archivo binario de índice (Nico)
├─ Makefile
└─ README.md

````

---

## ⚙️ Compilación y Limpieza
```bash
make          # Compila todos los módulos
make clean    # Limpia binarios y temporales
````

---

## 🚀 Ejecución del Sistema

Abrir **dos terminales**:

### 🖥️ Terminal 1 — Servidor

```bash
./p2-searchd
```

### 💻 Terminal 2 — Cliente

```bash
./p2-dataProgram
```

---

## 🔁 Flujo de Comunicación

El cliente y el servidor se comunican mediante **sockets TCP** y se sincronizan con **semáforos POSIX** (`sem_open`, `sem_post`, `sem_wait`):

```
Servidor                         Cliente
---------                        ---------
sem_open()  <------------------  sem_open()
socket()
bind()
listen()
sem_post()  ------------------>  sem_wait()
accept()
read()       <-----------------  write()
write()      ----------------->  read()
close()                         close()
```

---

## 🧩 Funcionalidades del Cliente (Misael)

📄 **Archivo:** `p2-dataProgram.c`

El cliente maneja el menú principal, la conexión al servidor y la interacción con el usuario.

### Menú Interactivo

1. **Realizar búsqueda (paginada con flechas)**

   * Envía el comando `FIND|q=<cadena>` al servidor.
   * Muestra resultados como `[Resultado #1]`, `[Resultado #2]`, etc.
   * Navegación por teclado:

     * `→` siguiente
     * `←` anterior
     * `q` salir

2. **Escribir un registro**

   * Envía un nuevo registro para añadirlo al dataset.
   * El servidor lo guarda y el indexador (Nico) actualiza el índice.

3. **Leer por número de registro**

   * Permite leer una línea específica del dataset.

4. **Salir**

   * Cierra la conexión y libera recursos (semáforo y socket).

---

## 🖥️ Funcionalidades del Servidor (Tuli)

📄 **Archivo:** `p2-searchd.c`

* Inicializa el socket del servidor y gestiona las conexiones entrantes.
* Crea el semáforo global (`SEM_NAME`) y publica (`sem_post()`) cuando está listo.
* Atiende comandos del cliente (`FIND`, `WRITE`, `READIDX`).
* Llama a funciones de indexación y lectura en el CSV.
* Devuelve respuestas formateadas mediante `write()`.

---

## ⚙️ Funcionalidades del Indexador (Nico)

📄 **Archivos:** `index.c`, `hash.c`, `index.h`, `hash.h`, `index.bin`

### Descripción General

El módulo indexador se encarga de **optimizar el acceso** a los registros del dataset.
Permite búsquedas rápidas mediante un índice binario y actualiza los datos de forma dinámica.

### Funciones Principales

1. **Construcción del índice (`indexar`)**

   * Recorre el CSV y genera `index.bin` con los offsets de cada registro.

2. **Búsqueda por título (campo indexado)**

   * Usa funciones hash para localizar un registro sin recorrer todo el archivo.

3. **Reindexación dinámica**

   * Cuando el cliente agrega un nuevo registro, se actualiza `index.bin` para incluirlo.

### Ejemplo de Flujo

1. El cliente envía un nuevo registro.
2. El servidor lo escribe en `dataset.csv`.
3. El módulo de Nico actualiza `index.bin` con el nuevo offset.
4. Las búsquedas posteriores usan el índice actualizado, sin leer todo el CSV.

---

## 🧵 Sincronización y Seguridad

* Comunicación bidireccional segura con `read()` / `write()`.
* Control de concurrencia mediante **semáforos POSIX**.
* Uso de `fcntl()` para bloqueo de escritura en el dataset.
* Cierre ordenado de conexiones para evitar sockets huérfanos.

---

## ✅ Pruebas Realizadas

* Comunicación Cliente-Servidor estable.
* Envío y recepción correcta de comandos.
* Escritura de nuevos registros con reindexación automática.
* Navegación por resultados paginada.
* Sin conflictos de acceso ni errores de sincronización.

---

## 🧾 Notas Finales

* Desarrollado y probado bajo **Ubuntu (WSL)**.
* El dataset completo de 3.9 GB **no se incluye** en el repositorio.
* Para pruebas, se usa una muestra pequeña (`dataset.csv`).
* Los binarios (`p2-dataProgram`, `p2-searchd`, `index.bin`) **no deben subirse** a GitHub.

---

## 👨‍💻 Autores

* **Misael Jesús Flórez Anave:** Implementó el cliente, la comunicación por sockets y el control interactivo.
* **Tuli Peña Melo:** Implementó el servidor, el manejo de semáforos y la lógica de coordinación.
* **Nicolay Prieto Mendoza:** Desarrolló la indexación binaria, el manejo de hash y la reindexación dinámica de registros.

