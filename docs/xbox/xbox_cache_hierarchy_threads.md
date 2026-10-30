# Cachés jerárquicas y trabajo multihilo: investigación y propuesta

Fecha: 2026-10-28. Estado: diseño basado en código; no implementa residencia nueva
ni certifica ahorro o rendimiento. Complementa la [fase 0](xbox_memory_phase0.md)
y la [comparación inicial con Cemu](xbox_memory_cemu_feasibility.md).

Base Jadyx: `52c95fae381fbc30a0aac3d13dba0850fa774b84`, con los cambios locales
de fase 0 y compilación conservados. Base Cemu UWP, inspeccionada sin modificar:
`D:\Cemu UWP`, `a21da611082ae728eac23da7589355f551c0331e`.

## Resultado

Es viable compartir un coordinador de presupuesto, prioridades y predicción
entre pipelines y JIT. Cada backend necesita su propio protocolo de publicación
y retiro. La primera implementación debe ampliar la caché DXIL existente y
acotar los trabajos pendientes. La expulsión parcial de código JIT requiere
cambios adicionales en Dynarmic; agregar hilos o dividir archivos no la habilita.

Interpretamos «prewarn» como **prewarm**: preparar antes de la demanda.
**Precooling** designa aquí dejar de promover un grupo, cancelar preparación
especulativa y pasar por una gracia antes de retirarlo. Son políticas propuestas.

El objetivo medible es mantener margen dentro del límite UWP efectivo, incluido
el pico de carga y descarga simultáneas. Los 5120 MiB comprobados al arrancar
0.3.9.2 como Game no son un presupuesto exclusivo para cachés. No hay captura
de gameplay nueva que permita dimensionar niveles o demostrar ahorros.

## Qué existe realmente en Cemu

Rutas relativas a `D:\Cemu UWP`:

| Mecanismo observado | Evidencia | Uso en Jadyx |
| --- | --- | --- |
| Expulsión de DXIL con respaldo en disco; recarga al necesitarlo | `src/Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.cpp`, `EvictIdleDXIL` y `EnsureDXILResident` | Primera referencia para separar bytecode residente de identidad persistente |
| Límite compartido de compilaciones DXC/PSO en vuelo | Mismo archivo, `NativeCompilationGate` y `NativeCompilationSlot` | Acotar memoria temporal además del número de hilos |
| Recetas Core/Frequent/Occasional, uso por sesiones y poda | `D3D12PipelinePrewarmPolicy.h`, `D3D12PipelineStableCache.cpp` en ese directorio | Jerarquía por historial, sin atribuir semántica geográfica |
| Suspensión de preparación bajo presión y retiro diferido de PSO | `D3D12RendererMemory.inl` | Histéresis y conservación hasta completar el uso GPU |
| Un trabajador JIT, cola y validación de invalidaciones antes de publicar | `src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp` | Revalidación de generación tras compilar fuera del lock |

Hallazgos que limitan un port directo:

- `PPCRecompiler_deleteFunction` desconecta entradas y rangos, pero conserva el
  comentario `todo - free x86 code`. No es un modelo de expulsión física de JIT.
- `PPC_REC_ALLOC_BLOCK_SIZE` de 4 MiB corresponde a rangos de direcciones guest
  de las tablas; no prueba una caché de código nativo expulsable de ese tamaño.
- `BackendX64/BackendX64.cpp` tiene slabs y una ruta de doble mapeo RW/RX,
  pero `s_uwpDualMappingDisabled` está en `true`. El comentario registra crashes
  Xbox y causa desconocida. No trasladar esa ruta como solución validada.
- `docs/phase5-multithreaded-recording-plan.md` contiene avances S2a–S2c,
  pero S2d sigue pendiente. El código mantiene un `m_recordThread`.
  Preparación/emisión separadas conceptualmente no equivalen a dos etapas
  ejecutándose ya en paralelo.
- Core es una heurística por sesiones o `mustBeReady`; no identifica terreno,
  LOD ni región. Sus umbrales de memoria no se deben copiar sin medir Jadyx.

## Punto de partida de Jadyx

- [PipelineCache](../../src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp):
  hasta cuatro trabajadores de pipelines, prewarm separado y admisión por memoria.
  `LoadDiskResources` prepara DXIL incluso para PSO dormidos. Los jobs capturan
  entornos; limitar trabajadores no acota la memoria de toda la cola.
- [ThreadWorker](../../src/common/thread_worker.h): cola de tareas sin cota.
  Proponer admisión específica de cachés antes de cambiar el pool genérico.
- [DxilDiskCache](../../src/video_core/renderer_d3d12/d3d12_dxil_disk_cache.cpp):
  ya indexa offsets y carga payload en `Find`. Su `Open` puede truncar o reparar
  archivos; un inspector offline debe abrirlos exclusivamente para lectura.
- [GraphicsPsoKey](../../src/video_core/renderer_d3d12/d3d12_pso_key.h) conserva
  un propietario de bytecode y compara bytes completos ante colisiones.
  Liberar solamente `GraphicsPipeline::dxil` no libera todas las referencias.
  No sustituir esa comparación por igualdad de hash sin garantía equivalente.
- [GraphicsPipeline::Evict](../../src/video_core/renderer_d3d12/d3d12_graphics_pipeline.cpp)
  ya difiere la liberación hasta el retiro GPU. El hilo GPU es quien despierta,
  usa y expulsa pipelines. Debe conservarse ese contrato.
- [HotPipelineSet](../../src/video_core/renderer_d3d12/d3d12_pipeline_residency.h)
  guarda la última sesión; no constituye un historial de frecuencia o de zonas.
- [Prewarm paralelo JIT](../../src/core/arm/jit_prewarm_parallel.h):
  hasta cuatro propietarios, cada uno con su JIT detenido. No permite compilar
  sobre la misma instancia mientras ejecuta código guest.
- [Perfiles JIT](../../src/core/arm/dynarmic/jit_prewarm_profile.h) contienen
  descriptores, hashes, longitud y muestras; no código nativo reutilizable.
  `jit_prewarm_owner.cpp` valida contra rangos RX del programa cargado.
- Dynarmic, `src/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp`:
  chunks de 4 MiB y devolución de memoria después de `ClearCache`.
  Esto no permite desmapear un grupo arbitrario mientras está activo.
  `emit_x64.cpp` mantiene enlaces que deben deshacerse al invalidar.

## Identificación sin arrancar juegos

| Datos disponibles | Qué puede obtener un inspector | Qué no puede concluir |
| --- | --- | --- |
| `d3d12_dxil.bin` | Versión, generación, clave de 128 bits, offset, tamaño; checksum leyendo payload por streaming | Zona, prioridad real o toda la relación con recetas |
| `d3d12.bin` y `d3d12_hot.bin` existentes | Inventario de recetas y última sesión observada, con parser específico de cada formato | Todas las variantes futuras o frecuencia que no se registró |
| `jit-profile/<title>/core-N.bin` y variante A32 | Título/build/core, descriptor, hash, longitud, muestras; comprobación estructural | Código máquina ejecutable ni validez frente a bytes guest ausentes |
| Imagen ejecutable disponible sin ejecutar el título | En una fase posterior, validación de rangos y hashes usando un loader offline | Cobertura completa del código dinámico o orden futuro de ejecución |
| Ningún archivo previo | Solo catálogo vacío y políticas generales | Pipelines y bloques específicos aún no descubiertos |

El primer inspector puede trabajar con archivos exportados y fixtures sintéticos,
sin iniciar juegos ni crear un dispositivo D3D12. Debe limitar tamaños, contar
duplicados, comprobar truncamientos y distinguir «estructura válida», «checksum
válido» e «identidad validada contra el programa». No modificar cachés al analizar.

La identidad persistente propuesta incluye título/build, versión del formato y
generación del traductor. Para pipelines: estado normalizado, shaders y opciones;
la root signature necesita una identidad estable, nunca su puntero en memoria.
Para JIT: ISA A32/A64, descriptor de estado, identidad del módulo y hash/longitud
del código. Parches/mods que cambian código deben invalidar la correspondencia.
Dirección host, offset de archivo, instancia por núcleo y generación de residencia
son ubicaciones o contextos, no sustitutos de la identidad.

## Jerarquía propuesta

Separar **prioridad lógica** de **representación residente**. Un grupo compartido
puede tener recetas en RAM y DXIL en disco; otro puede tener PSO listo y bytecode
descargado si ninguna operación CPU lo necesita.

| Nivel lógico | Política |
| --- | --- |
| Esencial | Prelude, helpers y objetos necesarios para progresar; pin explícito y mínimo |
| Compartido frecuente | Alta prioridad por uso transversal; presupuesto acotado, no pin infinito |
| Activo | Conjunto utilizado recientemente, referencias/pins durante su uso |
| Transición | Gracia para retorno o TP; preparación limitada de posibles sucesores |
| Frío | Solo índice y respaldo persistente; reconstrucción bajo demanda |

Agrupar por co-uso temporal y dependencias cuando exista telemetría. «Llanuras
de Hyrule» no se deduce de un hash. Un shader puede compartirse entre interfaz,
terreno y varias regiones. Contar objetos compartidos una sola vez y no expulsar
sus dependencias mientras otro grupo las utiliza. Los LOD y las texturas son
recursos distintos: expulsar pipelines no descarga por sí mismo geometría o LOD.

Los grupos lógicos no tienen por qué coincidir con segmentos físicos JIT de
4 MiB. Mezclar código caliente y frío en un segmento impide devolverlo completo;
esa fragmentación debe medirse antes de fijar tamaño y política de asignación.

## Hilos, admisión y sincronización

Un coordinador decide prioridades y reservas; no emite comandos GPU ni modifica
un JIT activo. Reutilizar los workers existentes con colas acotadas por **cantidad
y bytes**, deduplicación por identidad y cancelación de trabajo especulativo.
No crear un hilo por bloque.

Prioridades: demanda necesaria para progresar, recarga, prewarm probable y
mantenimiento. Reservar capacidad para demanda; el prewarm no puede consumir
todos los permisos ni generar una espera circular. No esperar otro job del
mismo pool mientras se retiene el único permiso que necesita ese job.

Admisión propuesta: límite UWP menos uso real, reserva para picos y reservas de
trabajo admitido que todavía no se reflejan en ese uso. No sumar nuevamente el
ledger o la memoria GPU al uso del proceso. Contabilizar entradas en cola,
entornos, temporales DXC/JIT y objetos pendientes de retiro. Solo aumentar
concurrencia cuando exista margen; empezar conservador y ajustar con medidas.

Estado por objeto/generación:

```text
Indexado -> EnCola -> Cargando -> ListoCPU -> Compilando -> Residente
                 \-> Cancelado/Error                       |
Frío <--------- Retirado <--------- EnRetiro <--------- Enfriando
```

Publicación atómica del resultado completo; espera por objeto, sin locks globales
durante I/O, compilación o espera de fences. Los fallos y cancelaciones despiertan
a los consumidores. Revalidar generación antes de publicar: un resultado viejo
no debe revivir datos invalidados. Una demanda durante retiro usa una referencia
todavía válida o crea una generación nueva; nunca reutiliza memoria ya retirada.

Hay cuatro eventos distintos: tarea CPU terminada, comandos registrados,
comandos enviados y GPU completada. El retiro de JIT añade otro: ningún núcleo
puede seguir ejecutando o entrar al segmento. No intercambiar estos eventos.

Microsoft exige no operar simultáneamente sobre una misma command list o
allocator. Mantener el propietario actual del scheduler en esta etapa;
[el ejemplo oficial de multihilo D3D12](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-multithreading-sample-win32/)
no justifica compartir esos objetos libremente entre workers.

## Precooling y retiro seguro

**DXIL/PSO:** dejar de promover, cancelar carga especulativa no iniciada, esperar
gracia y retirar. Para DXIL, exigir copia persistida válida y cero lectores CPU;
resolver también propietarios del índice PSO y cachés de shaders enlazados.
El PSO conserva su vida hasta completar el último uso GPU mediante el scheduler.
«Programado para liberar» no se registra como memoria ya disponible.

**JIT, primera etapa:** preparar inventarios e I/O en background; admitir
precompilación solo con el propietario detenido según el contrato existente.
No llamar `PrecompileBlock` concurrentemente con la ejecución de esa instancia.

**JIT, segmentación futura:** cada segmento necesita generación, bloques,
enlaces entrantes/salientes y estado de retiro. Antes de devolver páginas:
impedir nuevas entradas, deshacer enlaces directos, invalidar dispatch/RSB,
alcanzar un punto seguro en todos los ejecutores afectados y retirar metadatos
asociados. Después se puede desmapear/reutilizar y publicar otra generación.
La frecuencia medida solo en dispatch subestima bloques alcanzados por enlaces
directos; instrumentar o muestrear con ese sesgo explícito.

Guardar código nativo en disco requiere además relocaciones, helpers, ABI,
características CPU y tratamiento de metadatos de unwind. No serializar chunks
como si fueran portables. Al generar/modificar código se debe cumplir también
la coherencia de instrucciones indicada por
[FlushInstructionCache](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-flushinstructioncache).

Usar histéresis y gracia temporal para evitar expulsar/recargar continuamente.
Frente a presión fuerte, cancelar primero lo especulativo; una transición no
autoriza liberar recursos aún utilizados. La predicción inicialmente observa
sin actuar, para medir aciertos, fallos y coste antes de asignarle memoria.

## Orden de implementación y condiciones de aceptación

1. **Catálogo offline de solo lectura.** DXIL y perfiles JIT primero, recetas
   después de explicitar su formato. JSON con identidad, tamaños, validación y
   relaciones conocidas/desconocidas. Fixtures válidos/corruptos/truncados,
   límites estrictos y prueba de que los archivos fuente no cambian.
2. **DXIL recargable y cola acotada.** Separar identidad de bytes residentes,
   conservar deduplicación exacta, lectores y respaldo. Medir bytes únicos
   liberados/recargados, pico de cola, latencia de demanda y tareas canceladas.
   Probar recarga, fallo de disco, colisiones y retiro compartido con fences.
3. **Jerarquías y prewarm adaptativo.** Registrar co-uso y frecuencia real;
   evaluar la predicción en modo observación. Presupuestos independientes
   para residencia, compilación y transición, subordinados al límite global.
4. **Prototipo JIT por segmentos.** Harness con código sintético, enlaces entre
   segmentos, invalidación durante compilación, retorno a bloques expulsados
   y estrés multihilo. Exigir cero entradas a generaciones retiradas y devolución
   física de páginas antes de integrarlo al runtime normal.
5. **Validación Series.** Homebrew para corrección inicial y títulos reales
   para memoria/rendimiento: recorrido, TP y retorno repetidos; comparar pico
   UWP, margen mínimo, tiempos p95/p99, misses, bytes recargados y recompilaciones.
   Los fixtures offline no certifican estabilidad ni ahorro durante gameplay.

Primera entrega recomendada: catálogo offline más contrato de identidad y
presupuesto; después DXIL recargable. No empezar por una nueva arquitectura de
recording GPU ni por persistir código nativo JIT. La investigación deja esas
dependencias explícitas sin modificar todavía el comportamiento del emulador.

## Entrega 1: inspector offline (implementado, 28oct2026)

`tools/xbox/inspect-caches.py` inventaría copias exportadas de `d3d12_dxil.bin`
(JDXC v2) y perfiles JIT (`core-N.bin` EDJITP02/01, `a32-core-N.bin` EDJA3201) sin
iniciar juegos ni crear dispositivo D3D12. Abre solo `rb`; nunca repara ni trunca
(al contrario que `DxilDiskCache::Open`).

```
python -I tools/xbox/inspect-caches.py <archivo|directorio>... [--output inventario.json] [--no-checksum]
python -I tools/xbox/tests/inspect-caches.py
```

Salida JSON por archivo: formato/versión/generación, título/build/núcleo solo si el
archivo los contiene (DXIL no guarda título: `null`), offsets/tamaños/claves por
registro, registros sustituidos (Open conserva el último por clave), grupos con
mismo checksum+tamaño (indicio, no prueba de bytes iguales), etapas DXBC del
payload, estado estructural, checksum (`valid`/`mismatch`/`not_checked`) y
relaciones conocidas/desconocidas. `program_validation` es siempre
`not_available`: la clave DXIL depende del SPIR-V y la huella JIT de código guest
cargado en su base. Código de salida 1 si hay estructura inválida o checksum malo.

Memoria acotada: CityHash64 se calcula en streaming (cola de 64 B + chunks de
1 MiB), verificado contra `src/common/cityhash.cpp` compilado con MSVC en 18
tamaños (0 a 2 MiB+64). Perfiles JIT leídos por bloques; no se guardan registros.

Límites: no lee `d3d12.bin` ni `d3d12_hot.bin` (salen como `unknown`); la cola
stream-output de un payload DXIL no tiene longitud y queda en `trailing_bytes`;
el número de muestras no es prioridad real ni cobertura. Sin probar aún contra
cachés reales exportadas de la Series.

Siguiente paso: exportar cachés de un título y ejecutar el inspector; después,
DXIL recargable con identidad separada de bytes residentes.

Exportación real 28oct2026 (0.3.9.2, título 0100187003a36000, en
`build-uwp/cache-export/2026-10-28_20-25-44`): DXIL 301 registros/2,9 MB,
estructura y checksums válidos, sin claves repetidas. Tras volver al menú
(cierre ordenado) tres perfiles A64 EDJITP02 válidos: core0 104010, core1
168461, core2 119180 registros; mismo build id; 0 registros con muestras de
gameplay, así que no informan prioridad. Matar la app no guarda perfiles JIT
(solo `~ArmDynarmic64/32`).

## Entrega 2: DXIL recargable (implementado, sin probar en Series)

Opt-in `boot.cfg` `dxil_release=1` (por defecto apagado: comportamiento previo).

- Identidad separada de bytes: al crear un pipeline, si el registro está en el
  disco DXIL (`ShaderCompiler::StoredPipelineKey` + `DxilDiskCache::Contains`),
  el pipeline guarda `reload_dxil` (clave de disco + índices de etapa + xfb).
  Sin registro (caché apagada, Store fallido) no hay recarga y se conservan bytes.
- Liberación: en el barrido de residencia, tras `Evict`, un pipeline `Dormant`
  inactivo suelta su referencia (`GraphicsPipeline::ReleaseDxil`, hilo GPU).
  Los bytes solo se liberan cuando caen el LRU `linked_shaders`, las claves del
  índice PSO (retenidas hasta que su PSO se retira por fence) y otros pipelines;
  el cargo `ShaderBytecode` sigue hasta entonces, nunca se cuenta antes.
- Recarga: `Wake` lee de disco en su worker (`LoadStoredPipeline`), nunca en el
  hilo GPU ni con locks globales. Fallo de lectura: BUG_TRACK y draws omitidos
  esa sesión (no hay SPIR-V para recompilar).
- Deduplicación: `ExactShareIndex` (weak, no retiene memoria) reutiliza la copia
  viva registrada para el registro; copias leídas de disco se comparten solo si
  los bytes y la declaración stream-output son iguales. La igualdad exacta del
  índice PSO no cambia.
- Log cada 60 s: "DXIL N released, M read back".

Validado: objetos UWP de los 6 TUs tocados compilan (ninja, sin enlazar ni
empaquetar); `tools/xbox/tests/exact-share-index.cpp` pasa (cl /W4 /WX).

Límites: no probado en runtime ni Series; la búsqueda por registro vivo confía
en la clave de disco igual que la caché actual; el hash de SPIR-V se recalcula
por pipeline con el flag activo; compute pipelines no cubiertos; memoria real
liberada sin medir. Siguiente: paquete con `dxil_release=1` y A/B de
`ShaderBytecode`/app_used en el mismo título, luego colas acotadas.

Medición 0.3.9.3 en Series (Game, 5120 MiB), mismo título, 28oct2026: 4 min sin
crash, pico 2840 MiB. Log: `DXIL 0 released, 0 read back`, `0 released and 0
rebuilt` PSO: sin presión, el barrido nunca corrió. DXIL total ~8 MiB frente a
texturas 560-712 MiB, DRAM 1,3 GiB y ~640 MiB "rest": en este juego DXIL no es
palanca. Recarga real sin ejercitar. Un primer intento crasheó por OOM a 1024 MiB:
el paquete nuevo perdió App type Game (reponer tras cada instalación de versión).
Nota: el portal lista tamaño 0 para logs abiertos; la descarga sí es completa.

## Entrega 3: ventana acotada en la carga de disco (implementado, sin probar en Series)

Hallazgo: `LoadDiskResources` encolaba en `workers` cada registro de `d3d12.bin`
con sus `FileEnvironment` deserializados, sin cota (`ThreadWorker` no la tiene).
La demanda no compite aquí: la carga termina (`WaitForRequests`) antes del
gameplay, así que la reserva para demanda no aplica a esta cola.

`BoundedAdmission` (`d3d12_bounded_admission.h`): ventana por cantidad (2x
workers) y bytes (32 MiB de registro serializado como aproximación de su
memoria). Se adquiere en el hilo de carga, nunca en un worker (sin espera
circular); cada tarea lleva su Ticket y lo devuelve al terminar, también si
lanza. Respeta `stop_loading`. Un registro mayor que la cota entra solo con la
ventana vacía. Log: "disk load queue peak N records, K KiB serialized
environments".

Validado: `tools/xbox/tests/bounded-admission.cpp` (cl /W4 /WX) y objeto UWP de
`d3d12_pipeline_cache.cpp`. Límites: bytes serializados ≠ memoria en mapas; las
colas de runtime (builds de demanda, Wake) siguen sin cota ni prioridad; en este
título `d3d12.bin` pesa 3,2 MB, así que el ahorro esperado es pequeño. Cotas no
medidas en Series.

## Estado TOTK en Series (30oct2026)

Mediciones (0100f2c0115b6000, App type Game, límite 5120 MiB):

| Versión | Cambio | Pico commit | Resultado |
|---|---|---|---|
| 0.3.9.4 | alloc_watch, cola acotada | 4801 | sin crash; caché vacía |
| 0.3.9.5 | jit_cache_mib=96 | 4795 | JIT 306 vs 335 MiB: casi nada |
| 0.3.9.6 | drenaje dirigido de heaps | 5022 | +heaps liberados, pico igual, más tirones: retirado |
| 0.3.9.7 | heaps de textura 32 MiB | 5038 | huecos 520→~250 MiB; texturas vivas 1052 en ráfaga |
| 0.3.9.8 | Emergency GC 4 descargas/80 iter | 5082 | texturas ~820 en ráfaga (OK); bad_alloc por caché caliente |
| 0.3.9.9 | DXIL de pipelines dormidos del arranque fuera | 5116 | OOM a 26,5 min; DXIL 167→36 MiB al arrancar, 87 al crashear |
| 0.3.10.0 | pipelines frías hechas al dibujarse; GC de buffers bajo presión | 5115 | 13,6 min; 35k objetos de pipeline para 3,3k PSOs |
| 0.3.10.1 | claves canónicas (depth bias, app_stage) | 5040 (solo en la carga) | estable 4760-4920 a los 12,6 min; 3864 objetos |

Dueños confirmados del heap: patch_information de Dynarmic (hasta 38,5 MiB/núcleo),
grabación de perfil JIT (14 MiB/núcleo), kernel emulado (~75 MiB), fuentes (17 MiB),
macros GPU (2 MiB c/u). PSO liberados devuelven 0 KiB de commit. Prewarm JIT no
actuó (sin perfiles en consola).

Causa principal del crecimiento: TOTK cambia la pendiente del depth bias casi en cada
draw (1552 valores cerca de -0,5) y la clave guardaba el float exacto. En la caché del
28oct: 15389 claves de 1177 juegos de shaders; sin el bias, 4283. La Series no tiene
depth bias dinámico (OPTIONS16), así que va en el PSO: `d3d12_key_canon.h` guarda el
bias entero que usa el PSO y la pendiente con 8 bits de mantisa (error <= 0,2 %), y
`Canonicalize()` también borra `app_stage` (solo Vulkan/OpenGL). Las claves de disco
se normalizan al cargar: 69464 registros quedaron en 7438. Análisis offline:
`keyvar.py` sobre `d3d12.bin` (formato de registro: envs serializados y clave de
1072 bytes; tamaños sacados de las cabeceras con un programa de prueba).

Otros cambios: en consola (límite < 8 GiB) sólo se crean al arrancar las pipelines con
PSO de arranque; las demás se hacen desde los shaders vivos al dibujarse, con su DXIL
del disco, y no se vuelven a escribir en `d3d12.bin`. El GC de buffers borra los que
llevan 20 frames sin uso cuando quedan menos de 384 MiB libres.

Pendiente:
- La primera sesión tras 0.3.10.1 preparó sólo 345 PSOs de arranque (el historial usaba
  las claves viejas); la siguiente debería volver a 4096 y bajar los tirones.
- Texturas: ráfaga de 1081 MiB vivas en la carga del mundo y ~200 MiB libres dentro de
  heaps (127 en los de render targets). TOTK recrea sus render targets de resolución
  dinámica cientos de veces (800x448 creado 883 veces): reutilizarlos es lo siguiente.
- Después: compactar patch_information, grabar menos perfil bajo presión.

Logs en `build-uwp/console-logs/0.3.9.*-totk-*` y `0.3.10.*-totk-*`.
