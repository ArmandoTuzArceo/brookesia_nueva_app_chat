# Análisis del proyecto y plan de cámara + galería

Análisis inicial: 25 de octubre de 2026. La fase 1 de guardado de fotos se implementó y compiló el 30 de octubre de 2026 con ESP-IDF 5.5.5. El repositorio Git está presente. Al iniciar esa fase no había un `sdkconfig` generado; el build creó uno ignorado por Git a partir de `sdkconfig.defaults`. Antes de flashear hay que confirmar que ese archivo coincide con la configuración que funciona en la placa.

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
- La versión utilizada para compilar la fase 1 fue ESP-IDF 5.5.5, como se solicita. No se cambió la versión del framework.

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
  -> solicita el siguiente fotograma
  -> el callback V4L2 copia RGB565 a un buffer JPEG independiente
  -> una tarea codifica JPEG y escribe en /sdcard/DCIM/Camera
  -> al terminar, la UI informa el resultado y actualiza la foto del álbum
```

La fase 1 ya conecta el botón con el codificador JPEG por hardware y la escritura FAT. Se guarda un archivo por toque en `/sdcard/DCIM/Camera/ESP_000001.jpg`, usando nombres secuenciales, una calidad JPEG inicial de 80 y un archivo temporal que se renombra al terminar. El botón se deshabilita mientras la operación está activa. Si se cierra Camera después de solicitar la captura, el stream se detiene después de que el callback haya podido copiar y encolar el fotograma; la tarea de escritura no depende de la pantalla ni del buffer V4L2.

Aspectos relacionados con la cámara que siguen fuera de la fase 1:

1. La UI generada fija el canvas de preview a 1280 x 720, distinto del fotograma configurado (1288 x 728).
2. `Camera::run()` vuelve a solicitar la configuración de los buffers al reabrirse la app; el ciclo completo de cerrar y volver a abrir debe validarse en la placa.
3. La detección IA y la liberación de tareas, eventos, cliente PPA y buffers requieren una revisión independiente.

## 5. Almacenamiento de fotos: diseño recomendado

Usar una carpeta propia, nunca la raíz de la tarjeta:

```text
/sdcard/DCIM/Camera/ESP_000001.jpg
/sdcard/DCIM/Camera/ESP_000002.jpg
/sdcard/DCIM/.thumbs/ESP_000001.jpg       (fase posterior)
```

La implementación actual busca el mayor índice `ESP_*.jpg` existente y usa el siguiente, por lo que no necesita SNTP ni contador NVS. Escribe a un `.tmp`, comprueba `fwrite`/`fflush`/`fclose` y renombra al `.jpg` definitivo; la galería debe ignorar temporales incompletos.

La imagen de cámara es RGB565, de 1 875 328 bytes por foto (1288 x 728 x 2). La fase 1 implementa esta operación:

1. Copiar el fotograma V4L2 a memoria alineada con `jpeg_alloc_encoder_mem`.
2. En una tarea de guardado, crear el codificador JPEG del P4.
3. Codificar RGB565 a JPEG YUV422, calidad 80.
4. Reservar un buffer de salida de hasta dos veces el tamaño RGB565 y escribir únicamente los bytes indicados por `out_size`.
5. Actualizar la miniatura y notificar éxito/error desde el ciclo de LVGL.

Los búferes de entrada y salida se reservan con `jpeg_alloc_encoder_mem`, no con `malloc` normal: el driver exige alineación para JPEG/2D-DMA. Si la reserva o la codificación falla, se informa el error sin escribir un JPEG final incompleto.

El servicio reside en `components/apps/camera/PhotoStorage.hpp/.cpp`; expone el estado de la captura, acepta un solo trabajo a la vez y conserva la tarea de escritura aunque Camera se cierre.

La prueba con tarjeta, lectura del JPEG en un PC, secuencia de capturas, falta de espacio y salida durante la codificación siguen pendientes en el dispositivo físico.

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

1. **Completado en código:** guardador JPEG, creación de `DCIM/Camera`, nombres secuenciales, archivo temporal y notificación a Camera.
2. **Completado:** build con ESP-IDF 5.5.5 y configuración efectiva generada desde `sdkconfig.defaults`; no se flasheó el dispositivo.
3. **Pendiente en placa:** confirmar montaje/resolución por monitor, guardar y abrir una foto, capturas repetidas, SD llena y volver a inicio durante el guardado.
4. Crear Gallery como app separada, registrar su icono, incluirla en `apps.h` e instalarla en `main.cpp`.
5. Implementar escaneo, orden y visor de una foto; después borrado y actualización; por último miniaturas, favoritos, álbumes, selección múltiple o compartir por Wi-Fi.

Pruebas mínimas para cada versión: tarjeta ausente, tarjeta FAT32 vacía, carpeta inexistente, una foto válida, diez fotos, JPEG corrupto, SD llena, volver a inicio durante una codificación y reinicio durante la escritura. El resultado correcto es un mensaje recuperable, no un bloqueo de LVGL ni un reinicio.

## 8. Archivos modificados en la fase 1

```text
components/apps/camera/Camera.hpp             # estado UI y recursos de foto
components/apps/camera/Camera.cpp             # solicitud/snapshot, estado y ciclo de cierre
components/apps/camera/PhotoStorage.hpp       # estado/API de guardado
components/apps/camera/PhotoStorage.cpp       # JPEG + FAT + tarea de escritura
```

`components/apps/CMakeLists.txt` ya recopila estos archivos mediante `file(GLOB_RECURSE ...)`. Gallery no forma parte de esta fase.

## Referencias técnicas consultadas

- ESP-IDF 5.5, JPEG para ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/jpeg.html
