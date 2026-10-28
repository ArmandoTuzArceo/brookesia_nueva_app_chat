# Análisis del proyecto y plan de cámara + galería

Fecha del análisis: 25 de octubre de 2026. La copia analizada contiene 1 044 archivos; 735 son fuentes, cabeceras o archivos de configuración. No hay carpeta `.git` ni `sdkconfig` generado en esta copia, por lo que no es posible distinguir cambios locales de los de origen ni confirmar la configuración efectiva de la última compilación.

## 1. Mapa real del proyecto

| Zona | Responsabilidad | Qué debe modificarse para fotos/galería |
|---|---|---|
| `CMakeLists.txt` | Inicializa ESP-IDF y empaqueta `spiffs/` como la partición `storage`. | No guardar fotos aquí: SPIFFS está pensado para recursos incluidos con el firmware. |
| `partitions.csv` | Flash de 16 MiB: `factory` 7 MiB y `storage` SPIFFS 8 MiB. | No interviene en la SD. Conviene mantener las fotos en FAT de la microSD. |
| `sdkconfig.defaults` | Perfil reproducible de P4, PSRAM, pantalla, cámara y SD. | Ya habilita `CONFIG_EXAMPLE_ENABLE_SD_CARD=y`; falta preservar el `sdkconfig` real que funciona en la placa. |
| `main/main.cpp` | Arranque, montaje de archivos, pantalla y registro de las apps. | Monta SPIFFS y SD, e instala Camera, Image y el resto de apps. Aquí se registrará Gallery. |
| `components/apps` | Un único componente que compila todas las aplicaciones. | Añadir `gallery/Gallery.hpp` y `gallery/Gallery.cpp`; incluirlo en `apps.h` e instalarlo en `main.cpp`. |
| `components/espressif__esp32_p4_function_ev_board` | BSP: display, touch, audio, SDMMC de cuatro bits y SPIFFS. | Es el dueño de `bsp_sdcard_mount()` y del punto de montaje `/sdcard`. No hace falta duplicar su controlador. |
| `components/bsp_extra` | Audio y un adaptador de `esp-file-iterator`. | Puede servir de ejemplo para archivos, pero Gallery debe usar un escaneo propio para filtrar, ordenar y mostrar metadatos. |
| `components/espressif__esp-brookesia` | Framework Phone: launcher, ciclo de vida, gestos, barra y recientes. | Usar su clase `ESP_Brookesia_PhoneApp`; no modificar el framework para añadir una app. |
| `components/esp_cam_sensor` | Controladores genéricos de sensores, incluidos OV02C10 y SC2336. | No debe cambiarse para guardar una foto: Camera recibe ya un fotograma RGB565 procesado por el ISP. |
| `components/human_face_detect`, `pedestrian_detect` | Modelos ESP-DL y sus envoltorios. | Son independientes de captura/almacenamiento; Camera los usa sólo en los modos de IA. |
| `components/ethernet_init` | Inicialización Ethernet opcional. | Sin relación con las fotos. |

Los directorios `assets/`, las fuentes tipográficas grandes, los `.png` y sus `.c` correspondientes contienen recursos convertidos para LVGL. No conviene editarlos a mano. Los `.c` de iconos exportan un `lv_img_dsc_t`; para una app nueva se genera un icono PNG y se convierte con la herramienta de LVGL/SquareLine. Los modelos `.espdl` son binarios de IA empaquetados durante la compilación, y los cientos de ficheros de `esp_cam_sensor/sensors/` son registros y modos de sensores alternativos; no participan en el flujo de la cámara activa salvo el OV02C10 seleccionado en Kconfig.

## 2. Configuración de esta placa

La configuración guardada declara lo siguiente:

- Objetivo `esp32p4`, PSRAM activa y pantalla RGB565.
- Cámara `OV02C10`, RAW10, 1288 x 728 a 30 fps (`CONFIG_CAMERA_OV02C10_MIPI_RAW10_1288X728_30FPS=y`). El comentario de `Adaptacion.txt` menciona SC2336 y dos lanes, pero el código que se compila y el Kconfig seleccionan OV02C10 en **un** lane. La fuente de verdad debe ser el `sdkconfig` generado en el equipo que funciona.
- `CONFIG_EXAMPLE_ENABLE_SD_CARD=y`; el BSP expone la tarjeta como `/sdcard` por SDMMC de cuatro bits.
- La cabecera del archivo dice ESP-IDF 5.5.5, mientras que el entorno indicado es 5.5.6. La versión 5.5.6 es una actualización de correcciones de 5.5; conviene regenerar y guardar el `sdkconfig` con 5.5.6 antes de tocar la cámara.

El proyecto reserva una tabla de particiones de 16 MiB aunque el log de ejemplo del README llega a detectar un chip flash de 32 MiB. Eso no afecta a la SD, pero no debe cambiarse a ciegas: comprobar primero el tamaño real reportado al arrancar y decidir después si se rediseña la tabla de particiones.

## 3. Arranque y ciclo de vida de las apps

El recorrido al encender es:

```text
app_main
  ├─ NVS
  ├─ bsp_spiffs_mount()                 -> /spiffs
  ├─ bsp_sdcard_mount()                 -> /sdcard (FAT)
  ├─ display/LVGL + ESP_Brookesia_Phone
  └─ phone->installApp(...)             -> init() de cada aplicación
       └─ launcher abre una app          -> run(), pause(), resume(), back(), close()
```

`main.cpp` tiene comentadas las directivas `#if CONFIG_EXAMPLE_ENABLE_SD_CARD`, no el código de montaje. Por tanto intenta montar la SD siempre, incluso si se desactiva la opción de menú. El reproductor de vídeo se instala únicamente si ese montaje devolvió `ESP_OK`. La tarjeta no tiene pin de detección configurado (`SDMMC_SLOT_NO_CD`), así que no existe detección física de inserción/extracción; una galería debe ofrecer “Actualizar” y tratar los errores de E/S de forma segura.

`ESP_Brookesia_PhoneApp` es la clase base adecuada. Para Gallery se debe usar `ESP_Brookesia_PhoneApp("Gallery", &img_app_gallery, true)`: el framework crea/incluye la superficie normal de la app y llama a sus métodos de ciclo de vida. `init()` debe crear objetos persistentes y recursos de larga vida; `run()` debe refrescar el índice de fotos. No se debe crear una tarea o pantalla nueva cada vez que se abre la app sin destruir la anterior.

## 4. Cámara actual: qué hace y qué no hace

Archivos involucrados:

- `components/apps/camera/Camera.hpp/.cpp`: aplicación Camera, preview, UI y modos de IA.
- `components/apps/camera/app_video.h/.c`: adaptación V4L2 sobre `esp_video`; abre MIPI-CSI, solicita dos búferes y crea la tarea de streaming.
- `components/apps/camera/app_camera_pipeline.*`: colas para el flujo de inferencia.
- `components/apps/camera/app_*detect.*`: adaptan los detectores de personas y rostros.
- `components/apps/camera/ui/*`: UI generada con SquareLine; no guarda imágenes.

Flujo actual:
  
```text
OV02C10 RAW10 -> ISP/esp_video -> V4L2 RGB565 -> dos búferes en PSRAM
  -> camera_video_frame_operation()
     -> canvas LVGL (preview)
     -> opcionalmente detector IA y rectángulos

Botón de disparo
  -> pausa el evento de tarea
  -> memcpy del RGB565 actual a _img_album_buffer
  -> reanuda el preview y muestra esa copia en “Photo”
```

No hay llamada a `jpeg_new_encoder_engine`, `jpeg_encoder_process`, `fopen(..., "wb")`, `fwrite` ni `mkdir` en Camera. El botón **no produce un archivo**: sólo mantiene una copia RGB565 volátil para la miniatura y la pantalla Photo. El componente `apps` ya declara `esp_driver_jpeg`, por lo que no hace falta añadir otra dependencia para codificar JPEG.

Problemas que se deben corregir antes de ampliar Camera:

1. `_img_album_dsc` anuncia un tamaño de datos cuadrado (`min(ancho,alto)^2 * 2`) pero el búfer se reserva con el tamaño real 1288 x 728 x 2. El descriptor de la miniatura no corresponde con la imagen.
2. La UI generada fija el canvas de preview a 1280 x 720, distinto al fotograma configurado (1288 x 728). Se debe usar la resolución efectiva que devuelve V4L2 o un contenedor que escale el preview.
3. La copia del botón comparte memoria con la tarea de V4L2. Pausar un bit de evento no garantiza por sí solo que la devolución del búfer al controlador no esté ocurriendo a la vez. Es necesario un mutex o una sección de “snapshot” bien definida.
4. Comprimir y escribir a SD dentro del callback de LVGL o del callback de V4L2 congelaría el preview. La codificación y la escritura deben vivir en una tarea de trabajo, alimentada por una cola de longitud uno.
5. `Camera::run()` vuelve a crear la secuencia de inicialización al cerrarse su semáforo; al reorganizarla hay que separar “ya inicialicé el sensor” de “estoy visible”. También se debe liberar tareas, eventos, cliente PPA y búferes en `close()`.

## 5. Almacenamiento de fotos: diseño recomendado

Usar una carpeta propia, nunca la raíz de la tarjeta:

```text
/sdcard/DCIM/Camera/ESP_000001.jpg
/sdcard/DCIM/Camera/ESP_000002.jpg
/sdcard/DCIM/.thumbs/ESP_000001.jpg       (fase posterior)
```

Para los nombres, un contador persistido en NVS evita colisiones antes de que SNTP tenga hora. Cuando la hora sea válida se puede migrar a `YYYYMMDD_HHMMSS_###.jpg`, manteniendo el contador como desempate. La escritura debe hacerse en `archivo.tmp`, comprobar `fwrite`, vaciar/cerrar y renombrar a `.jpg`; así una desconexión o reinicio no deja una foto truncada que la galería intente decodificar.

La imagen de cámara es RGB565, de 1 875 328 bytes por foto (1288 x 728 x 2). Una operación segura es:

1. Copiar el fotograma a un búfer de snapshot alineado y dedicado.
2. En una tarea de guardado, crear el codificador JPEG del P4.
3. Codificar RGB565 a JPEG YUV422, calidad inicial 80.
4. Escribir únicamente los bytes indicados por `out_size`.
5. Actualizar la miniatura y notificar éxito/error en la UI desde el contexto protegido de LVGL.

Los búferes de entrada y salida deben reservarse con `jpeg_alloc_encoder_mem`, no con `malloc` normal: el driver exige alineación para JPEG/2D-DMA. Para una primera versión, reservar la salida con una capacidad conservadora de al menos el tamaño RAW (más un pequeño margen) y comprobar siempre el resultado. No se debe asumir que `raw_size / 10` cabe para todos los contenidos. La documentación oficial confirma RGB565 como entrada válida, la necesidad de esos búferes alineados y que la codificación se hace en el hardware JPEG del P4.

La estructura de servicio propuesta es:

```cpp
class PhotoStorage {
public:
    esp_err_t begin();                         // valida /sdcard y crea DCIM/Camera
    esp_err_t enqueueRgb565(const uint8_t *frame, uint16_t w, uint16_t h);
    bool isBusy() const;
    const char *lastPath() const;
};
```

`Camera::onScreenCameraShotBtnClick()` sólo haría una copia rápida al snapshot y llamaría a `enqueueRgb565`. Una tarea `photo_writer_task` hace la codificación y almacenamiento. Si `isBusy()` es verdadero, el botón muestra “Guardando…” y no acepta otra captura: es mejor rechazar una foto que agotar PSRAM o bloquear la cámara.

## 6. App Image existente y la nueva Gallery

`components/apps/image_display/ImageDisplay.cpp` es útil como prueba de que el decodificador JPEG ya está integrado, pero no es una galería lista para reutilizar:

- Busca `/spiffs/image`, no `/sdcard`.
- Usa `file_iterator` sin aplicar la extensión `.jpg` declarada en el archivo.
- Cambia de imagen automáticamente cada 5 segundos, además de usar gesto izquierda/derecha.
- Si no hay archivos, el índice puede quedar inválido.
- Reserva dos búferes de 1024 x 600 x 2; una foto de la cámara necesita aproximadamente 1.82 MiB tras el redondeo de JPEG, por lo que ese tamaño no basta.
- Lee el tamaño asignado completo en vez de verificar los bytes que realmente leyó, no libera su iterador y usa estado global que persiste entre aperturas.

La alternativa limpia es una nueva carpeta `components/apps/gallery/`, sin alterar ImageDisplay. Gallery tendrá dos pantallas:

1. **Cuadrícula/lista**: escanea `/sdcard/DCIM/Camera`, filtra `.jpg/.jpeg` sin distinguir mayúsculas, hace `stat`, ordena por fecha descendente y muestra nombre/tamaño/fecha. Al inicio puede ser una lista con tarjetas; es la forma fiable de validar todo el flujo sin decodificar decenas de fotos.
2. **Visor**: al tocar una entrada, lee un único JPEG, llama a `jpeg_decoder_get_info`, reserva `ALIGN_UP(ancho, 16) * ALIGN_UP(alto, 16) * 2` como mínimo con `jpeg_alloc_decoder_mem`, lo decodifica a RGB565 y lo muestra en un canvas. Incluye anterior/siguiente, borrar con confirmación y volver.

Las miniaturas son una segunda fase. La solución eficiente es producir en la captura un JPEG pequeño en `.thumbs`, o reducir con PPA y volver a codificar. Decodificar las fotos completas para las 20 celdas de una cuadrícula desperdicia PSRAM y tiempo. La app nunca debe precargar la tarjeta completa en RAM: mantiene sólo el vector de metadatos y, como máximo, el JPEG/bitmap de la imagen visible y un vecino.

## 7. Orden de implementación y pruebas

1. Guardar el `sdkconfig` que ya funciona y crear un repositorio Git antes del primer cambio. Confirmar por log que la resolución V4L2 es 1288 x 728 y que la tarjeta se monta en `/sdcard`.
2. Crear `PhotoStorage` con `begin()`, creación de directorios, contador NVS, escritura temporal y una prueba de archivo pequeño.
3. Añadir JPEG hardware y una tarea de guardado; conectar el botón de Camera; conservar la vista Photo actual como confirmación visual.
4. Validar en PC que cada `.jpg` abre, que los nombres no se repiten tras reiniciar y que diez capturas seguidas no reducen memoria libre de forma progresiva.
5. Crear Gallery como app separada, registrar su icono, incluirla en `apps.h` e instalarla en `main.cpp`.
6. Implementar primero escaneo, orden y visor de una foto; después borrado y actualización; por último miniaturas, favoritos, álbumes, selección múltiple o compartir por Wi-Fi.

Pruebas mínimas para cada versión: tarjeta ausente, tarjeta FAT32 vacía, carpeta inexistente, una foto válida, diez fotos, JPEG corrupto, SD llena, volver a inicio durante una codificación y reinicio durante la escritura. El resultado correcto es un mensaje recuperable, no un bloqueo de LVGL ni un reinicio.

## 8. Archivos que se modificarán en la primera entrega

```text
components/apps/camera/Camera.hpp             # estado y cola de captura
components/apps/camera/Camera.cpp             # snapshot y notificación al servicio
components/apps/media/PhotoStorage.hpp        # API de almacenamiento nueva
components/apps/media/PhotoStorage.cpp        # JPEG + FAT/NVS + tarea de escritura
components/apps/gallery/Gallery.hpp           # nueva app
components/apps/gallery/Gallery.cpp           # escaneo, visor y acciones
components/apps/gallery/assets/*              # icono LVGL de Gallery
components/apps/apps.h                        # include de Gallery
main/main.cpp                                 # phone->installApp(gallery)
```

`components/apps/CMakeLists.txt` no necesita una lista manual de esos archivos porque usa `file(GLOB_RECURSE ...)`. Aun así, todo símbolo no estático en C debe llevar prefijo o namespace para evitar colisiones entre apps.

## Referencias técnicas consultadas

- ESP-IDF 5.5, JPEG para ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/jpeg.html
- Publicación de ESP-IDF 5.5.6: https://github.com/espressif/esp-idf/releases/tag/v5.5.6
