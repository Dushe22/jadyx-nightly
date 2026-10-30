# Memoria: viabilidad de trasladar las técnicas de Cemu UWP

Investigación del 28 de octubre de 2026. Alcance: clonado, inspección de código,
contraste con fuentes primarias y diseño de experimentos. No implementa un port
ni certifica ahorros, FPS o resolución de OOM en consola.

Ampliación posterior sobre Jadyx `52c95fae381f`:
[cachés jerárquicas, identificación offline y multihilo](xbox_cache_hierarchy_threads.md).
La base indicada abajo corresponde a esta comparación inicial; el estado de
compilación e instalación actualizado está en la [fase 0](xbox_memory_phase0.md).

## Conclusión

Es viable reutilizar políticas de Cemu, especialmente la residencia de DXIL
respaldada por disco, la admisión del prewarm y el control de compilaciones en
vuelo. Jadyx ya contiene buena parte del sistema de presupuestos y residencia
de PSO: copiarlo entero duplicaría mecanismos. El candidato debe ampliar las
capas existentes, preservando identidad, sincronización y draws.

No se puede prometer que RAM deje de ser una limitación para cualquier juego.
El conjunto de datos simultáneamente necesario más los temporales debe caber
en el límite efectivo UWP. Una caché acotada evita crecimiento opcional, pero
no reduce por sí sola la memoria invitada activa ni las texturas necesarias.
El objetivo verificable es superar cargas concretas que hoy fallan, con margen
para picos y sin deteriorar la corrección ni los tiempos de frame.

## Base examinada

- Jadyx: `D:\Jadyx`, origin `https://github.com/JulianDr14/jadyx.git`, rama
  clonada `main`, HEAD `06693e4a2e3ca9bd718b0f8e7832b5aaa7182a0a`.
  La documentación histórica menciona `xbox`; no se cambió a una rama distinta.
- Cemu: `D:\Cemu UWP`, HEAD `a21da611082ae728eac23da7589355f551c0331e`;
  inspección del árbol local, sin cambios reportados por `git status --porcelain`.
- No hay logs nuevos de gameplay de esta investigación. Los números siguientes
  proceden del README y los cuadernos versionados; no son mediciones nuevas.

## Qué consume memoria y por qué pipelines no bastan

El [README de esta revisión](../../README.md#the-memory-wall) documenta para una
carga pesada: 2460 MiB de DRAM invitada, unos 1000 MiB de texturas activas,
750 MiB de heaps C++, 460 MiB de otros recursos GPU, 300–400 MiB de JIT y
200 MiB de ejecutables/DLL. Son categorías aproximadas de una captura PC,
no una suma exacta de asignaciones exclusivas en Xbox. El mismo documento
registra un pico PC sin límite de unos 6,4 GiB y unos 400 MiB de bloques grandes
todavía sin atribuir. Esos 400 MiB son un objetivo de investigación, no memoria
ya demostrada como prescindible.

El port ya evitó mantener unos 17000 PSO simultáneos (aproximadamente 1,3 GiB
según sus mediciones históricas). Ese ahorro ya está incorporado: no debe
contarse otra vez como beneficio futuro de portar Cemu.

Microsoft documenta 5 GB para juegos UWP Xbox en su página de recursos,
actualmente archivada. El port trabaja con 5120 MiB observados; para decisiones
en ejecución manda `AppMemoryUsageLimit`, no una constante ni la RAM física.
`ExpectedAppMemoryUsageLimit` anuncia un límite futuro posible, no disponible
de inmediato. Fuentes: [recursos Xbox UWP][ms-resources],
[límite esperado][ms-expected] y [presupuestos UWP][ms-budget].

## Comparación concreta con Cemu

| Mecanismo | Cemu local | Jadyx actual | Decisión |
|---|---|---|---|
| Presión global | `UwpMemoryBudgetService`: muestreo, eventos, histéresis, freno al trabajo pesado | `d3d12_cache_policy.h` separa app/DXGI; guard de staging y reserva antes de asignar | Conservar Jadyx; evaluar admisión común para trabajo opcional |
| PSO inactivos | `EvictColdGamePipelines`, historial y niveles de importancia | `TickResidency`, PSO gráficos compartidos y retiro por fence | Mejorar selección si el churn medido lo justifica |
| Bytecode inactivo | `EvictIdleDXIL` descarga RAM; `EnsureDXILResident` recupera de disco | `GraphicsPipeline` conserva `SharedDxilStages`; vaciar el LRU enlazado no elimina propietarios vivos | Diferencia principal que vale probar |
| Persistencia | DXIL y recetas con historial; blobs nativos vetados en Xbox | `d3d12.bin`, `d3d12_hot.bin`, `d3d12_dxil.bin` con identidad de traductor | Reutilizar archivos existentes, ampliar el descriptor recargable |
| Prewarm | Admisión antes/después de crear, margen y crecimiento desde baseline | Background exige >1536 MiB libres; selección inicial por historial, hasta 4096 PSO | Revisar admisión del arranque, no solo background |
| Compilación | Cupo compartido para DXC/driver, distinto carga/runtime | Hasta 4 workers de pipelines, más un worker de prewarm | Medir pico conjunto; un número de threads no es presupuesto en bytes |
| Texturas | Preserva recargas costosas y retira recursos seguros | ASTC→BC3/BC1, heaps placed, best-fit, GC/readback diferido y presupuestos | No reemplazar por un LRU genérico |

Referencias Jadyx: `src/video_core/renderer_d3d12/`:
`d3d12_pipeline_cache.cpp` (`LoadDiskResources`, `TickPrewarm`, `TickResidency`),
`d3d12_graphics_pipeline.cpp` (`Evict`), `d3d12_graphics_pipeline.h` (`dxil`),
`d3d12_linked_shader_cache.h` (`SetBudget`), `d3d12_dxil_disk_cache.{h,cpp}`,
`d3d12_pipeline_residency.h`, `d3d12_memory_guard.h` y `d3d12_pso_cache.h`.
En el arranque se prepara DXIL incluso para PSO dormidos; el gate de background
no limita por sí solo esas lecturas y compilaciones. La ruta compute posee DXIL
y PSO propios y no participa en el barrido gráfico: medir su peso antes de ampliarla.

Referencias Cemu: `src/uwp/UwpMemoryBudgetService.{h,cpp}` y
`src/Cafe/HW/Latte/Renderer/D3D12/`:
`RendererShaderD3D12.cpp`, `D3D12RendererMemory.inl`,
`D3D12RendererPipelines.inl`, `D3D12PipelineStableCache.cpp` y
`D3D12PipelinePrewarmPolicy.h`. Los porcentajes, tamaños y protecciones de
Cemu están ajustados a Wii U; no son valores óptimos demostrados para Switch.
También cambia el traductor: Cemu usa su ruta HLSL/DXC y Jadyx pasa de Maxwell
a SPIR-V y Mesa `spirv_to_dxil`, más firma DXIL. Se trasladan políticas y
estructuras de residencia, no archivos binarios de caché entre emuladores.

## Primera opción: experimento inspirado en Cemu

### 0. Baseline y presupuesto recuperable

Capturar la carga problemática con la misma versión, partida, mods, resolución,
historial JIT y cachés. Registrar consumo UWP y picos en cargas, ledger, heaps,
bytes DXIL únicos y compartidos, PSO gráficos/compute, temporales y retiros.
Separar liberación lógica de memoria efectivamente devuelta tras fences.
El tamaño real interno de PSO no lo expone D3D12: contar objetos y observar
deltas controlados; no multiplicar siempre por un supuesto de 100 KiB.

Calcular el déficit respecto al límite más una reserva experimental. Si el DXIL
recuperable no cubre ese déficit, no declarar que esta vía resolverá el caso.

### 1. DXIL con residencia acotada y recarga

Mantener metadata mínima de pipeline, identidad de compilación y localización
en `d3d12_dxil.bin`; cargar bytecode cuando se construya o despierte un PSO.
Compartir una única copia inmutable por identidad. Descargar solamente datos
recargables o regenerables, con compilaciones y diagnósticos activos protegidos.

El índice de PSO host también retiene propietarios de bytecode: expulsar una
referencia del pipeline puede no liberar nada mientras otra variante, el LRU,
una entrada compartida o un retiro siga viva. Hace falta contar bytes únicos
y diseñar la identidad para permitir reconstruirla sin mantener todo el DXIL.
No basta añadir `dxil.reset()` a `Evict`.

La caché en disco ya comprueba generación, tamaños y checksum. Un fallo de
lectura debe reconstruir desde entornos válidos con una ruta de publicación
que despierte waiters también ante error; conservar opciones, etapas, stream
output y transformaciones de fallback como el vertex flattening.
La clave de disco actual resume entradas: una revisión de persistencia debe
evaluar su política de colisiones/integridad; no confundir checksum con prueba
de equivalencia funcional. No cargar una variante distinta para evitar un stall.

### 2. Admisión de prewarm y temporales

Unificar contabilidad de trabajos opcionales en arranque/background; reservar
margen antes de crear y remedir después. Ensayar concurrencia 1/2/4 con el mismo
perfil y medir pico/tiempo de carga. Dar prioridad a demanda real frente a
prewarm, sin dejar al thread de render esperando un cupo cuyo dueño lo necesita
para terminar. Pausar prewarm no debe pausar un draw requerido indefinidamente.

Evaluar historial por recencia/frecuencia y coste observado de reconstrucción.
La protección de Cemu de pipelines imprescindibles no debe transformarse en
retención ilimitada en Jadyx. Todos los draws válidos deben conservarse.

### Gates y criterio para continuar

| Gate | Evidencia exigida |
|---|---|
| Identidad/recarga | Etapas, opciones y variantes exactas; disco ausente/truncado/incompatible; error y reintento sin deadlocks |
| Concurrencia/lifetime | Un build compartido, cierre/cambio de juego, referencias liberadas tras fence y memoria contabilizada una vez |
| GPU PC | Render/readback equivalente, debug limpio, ciclos repetidos de descarga y despertar |
| A/B de memoria | Mismo trayecto y cachés iniciales copiadas; pico, margen mínimo, bytes recuperados reales, I/O y recompilaciones |
| Series X y S | Carga problemática repetida, sesión prolongada manual, ausencia de OOM y errores; frame p95/p99/max y aspecto visual |

Como objetivo inicial, dejar al menos 256 MiB libres durante la carga; es una
reserva de experimento, revisable al medir el mayor pico, no garantía universal.
Comparar referencias y candidato por separado con sus propias cachés copiadas.
Un PC con Job de 5120 MiB no reproduce UMA: en GPU dedicada parte del consumo
vive fuera del commit limitado. El gate final es Xbox sin debugger que altere
las restricciones. No certificar FPS con un build o una prueba corta.

## Otras vías investigadas, por prioridad

1. **Atribuir bloques grandes C++ y capacidades retenidas.** Auditar por propietario
   los ~400 MiB históricos sin atribuir, tablas, buffers deswizzle, colas de carga y
   metadata. Separar vivos de capacidad libre retenida y fragmentación. Recortar
   temporales al terminar una carga puede ahorrar sin degradar imagen; todavía
   no se conoce qué parte es prescindible.
2. **Reducir el pico de conversión de texturas.** Decodificar/recomprimir por
   capas/mips o lotes acotados, reutilizando staging. Ryujinx contiene conversión
   ASTC→RGBA8→BC7 y evita algunas actualizaciones idénticas. La copia adicional
   que usa para comparar también consume memoria: no adoptarla sin límite.
   BC7 y BC3 ocupan ambos 16 bytes por bloque 4×4; cambiar BC3 a BC7 mejora
   potencialmente calidad, no reduce por sí solo tamaño. Jadyx ya usa BC3/BC1:
   el beneficio pendiente está en cobertura y temporales. [Código Ryujinx][ryu-texture].
3. **Texturas grandes con calidad configurable.** Reducir a la mitad ancho y
   alto puede aproximar un cuarto de bytes de imagen en formatos habituales,
   antes de alineaciones. Es distinto de bajar solo resolución de render.
   Probar primero texturas estáticas muestreadas; excluir depth, render targets,
   storage, readbacks y aliases hasta demostrar semántica. Cuesta nitidez y
   requiere adaptar dimensiones/LOD/vistas coherentemente. Mayor potencial
   cuando el conjunto activo ya no cabe, con calidad explícitamente optativa.
4. **Empaquetado y liberación real de heaps.** Medir ocupación simultánea, mayor
   hueco y recursos pendientes. Jadyx ya tiene best-fit/trim. D3D12MA muestra que
   desfragmentar exige recrear recursos, copiar y actualizar descriptores tras
   completar GPU; no es automático y puede necesitar memoria extra. Estudiar
   movimientos acotados solo si hay fragmentación demostrada. [D3D12MA][defrag].
5. **JIT/metadata bajo presupuesto.** Medir código realmente comprometido y
   metadata, ensayar límites menores por juego sin reducir DRAM invitada visible.
   Menos caché puede aumentar recompilaciones; prioridad posterior a bytes
   fríos de shaders y temporales. El PPTC de Ryujinx reduce traducción entre
   arranques, no elimina memoria necesaria para ejecutar código. [Ryubing][ryu-docs].
6. **GC según coste, no solo antigüedad.** Eden evita ciertas imágenes costosas
   en GC normal; su ruta GPU-dirty puede descargar y esperar `Finish`. Jadyx ya
   avanzó con GC diferido: aprovecharlo y medir churn, no copiar una espera
   síncrona adicional. [GC de Eden][eden-gc], [cuaderno local](xbox_texture_memory.md).

### Vías de baja prioridad o incompatibles con el primer experimento

- `GetCachedBlob` y bibliotecas nativas: Cemu veta ambas en Xbox por device
  removed; AGENTS de Jadyx prohíbe `GetCachedBlob`. El soporte documentado en
  PC no demuestra soporte seguro en Series. [CreatePipelineLibrary][pso-library].
- `Evict/MakeResident`: gestionar residencia GPU no equivale a ampliar la cuota
  UWP ni a swap de RAM invitada. Microsoft recomienda presupuesto y destrucción
  de heaps cuando se necesita recuperar memoria de forma fiable. En UMA no hay
  una VRAM separada donde desplazar gratuitamente datos. [Residencia][residency].
- Texturas sparse/tiled: exigen soporte del dispositivo y seguimiento de tiles,
  vistas, mips, aliases y fences; no todos los recursos son aptos. Evaluación
  posterior, no una sustitución directa del cache actual. [Residencia][residency].
- Comprimir/paginar DRAM invitada a disco: requeriría interceptar accesos CPU,
  GPU y servicios, resolver coherencia y controlar latencias y memoria pinned.
  No se identificó aquí una implementación de PC directamente trasladable a
  UWP. No asumir que `VirtualAlloc` reserve equivale a commit ni que RAM activa
  pueda descartarse; tampoco reducir los 4 GiB lógicos de Switch arbitrariamente.
- `HeapCompact`: no libera asignaciones vivas ni fuerza al driver a devolver
  PSO/texturas. Solo evaluar tras identificar memoria libre retenida; no usar
  como remedio global por frame ni como garantía de ahorro.

## Propuesta del usuario: conjuntos por zona y carga progresiva

El usuario propone mantener el bloque de la zona actual (ejemplo: llanuras de
Hyrule), descargar el anterior al teletransportarse y anticipar lo próximo al
avanzar. Es una dirección viable como gestión de conjunto de trabajo host;
no hay evidencia aún de que baste para la carga que falla.

Un pipeline describe cómo dibujar, no una zona geográfica: terreno, personajes
y efectos de zonas distintas pueden compartir PSO. Los LOD, objetos visibles
y streaming del mundo los decide el juego. Sin integración específica no se
puede inferir de una clave de pipeline qué está detrás de la cámara, ni omitir
sus draws. El emulador sí puede administrar las copias host reconstruibles de
recursos que el juego dejó de usar, conservando la memoria invitada correcta.

Diseño candidato, todavía sin implementar:

1. Registrar ventanas de uso efectivo de PSO/DXIL y texturas/buffers, con IDs
   estables durante su vida y generaciones que eviten confundir reutilizaciones.
   Mantener una base compartida de uso frecuente, un conjunto activo y una
   reserva pequeña para transiciones. Limitar también la metadata del predictor.
2. Inferir grupos de recursos que se usan juntos; no asignarles nombres de
   regiones ni asumir que cada grupo corresponde a una zona. Un cambio abrupto
   sostenido es señal de transición, no prueba de teletransporte. Menús,
   cinemáticas o cambios de cámara pueden producir señales parecidas.
3. Al cambiar el conjunto activo, detener prewarm obsoleto, descartar resultados
   opcionales de generaciones viejas y dar menor prioridad a recursos fríos.
   Retirar tras fences; preservar escrituras GPU pendientes y coherencia guest.
   Reutilizar rangos liberados antes de ampliar heaps; no duplicar dos zonas
   completas durante la transición.
4. Predecir PSO por transiciones observadas y recuperarlos de disco con una
   cuota pequeña de bytes/trabajo. No precargar imágenes desde memoria guest
   en workers sin validar mapeos, versiones y sincronización. La primera visita
   no tiene historial y conserva carga por demanda.
5. Mantener un período de gracia para girar la cámara o volver sobre los pasos.
   Usar tiempo real, recencia, frecuencia y coste de recarga; no solo frames o
   pertenencia rígida a un grupo. Los recursos compartidos se cuentan una vez.

Puntos existentes para instrumentar: `BuiltPipeline` actualiza `Touch` por uso,
pero `MarkDrawn` solo marca el primer draw de cada pipeline por sesión; ese
historial por sí solo no permite aprender secuencias de zona. `TickPrewarm`
recorre una lista preparada al boot. El GC genérico ya conoce antigüedad,
descargas necesarias y cargas costosas; añadir prioridad de conjunto debe
respetar esas condiciones, no sustituirlas.

Primer gate: predictor en observación, sin nuevas expulsiones ni precargas.
Comparar uso previsto/real, bytes únicos recuperables, falsos cambios de escena
y bytes que vuelven a necesitarse al girar/volver/abrir mapa/teletransportarse.
Después A/B por componentes: DXIL descargable, prioridades y finalmente
prewarm predictivo. Medir picos durante transición, no solo memoria estable.
Si los recursos simultáneamente activos exceden la cuota, el predictor no
resuelve ese mínimo: sigue haciendo falta reducir duplicación, temporales o
calidad opcional. No se elimina DRAM invitada por pertenecer a la zona anterior.

Referencia conceptual: Microsoft propone clasificar, presupuestar y hacer
[streaming de recursos](https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management-strategies).
Su ejemplo conoce visibilidad/distancia desde el motor; en el emulador esa
información semántica no está disponible automáticamente.

## Estado de validación

Actualización: [fase 0 implementada como diagnóstico](xbox_memory_phase0.md) y
cerrada con capturas de TOTK el 30 de octubre de 2026.

Fase 1, decisión del 30 de octubre: queda postergada. La parte de dormidas ya existe
desde 0.3.9.9 (`dxil_release=1`: `ReleaseDxil` tras `Evict`, recarga en worker).
Lo restante, el DXIL de pipelines con PSO residente, no se libera mientras
`GraphicsPsoKey` retenga los bytes para comparar claves completas; liberarlo exige
una identidad por hash de contenido (sin comparación byte a byte) para recuperar
<= 17 MiB en 0.3.10.4 (35 en el caso 0.3.9.9). No compensa ahora el riesgo de
reutilizar el PSO de otro shader ante un choque.

Fase 2, implementada en 0.3.10.6 (sin medir en consola todavía). Contexto medido:
el prewarm de pipelines en background nunca corría en consola (`PLENTY_FREE` pide
1,5 GiB libres) y de 10164 pipelines el arranque construye 4096; las 6068 diferidas
son los tirones de pipeline (65 de 158 en 0.3.10.5).

- `PrewarmAdmission` (`d3d12_pipeline_residency.h`): presupuesto de bytes por
  sesión y reserva de 256 MiB libres. Antes de cada build exige reserva + coste
  estimado (EWMA del delta medido en las cuentas `Pipelines` + `ShaderBytecode`,
  acotado a 4 KiB–1 MiB); si cae por debajo pausa y retoma con 64 MiB de margen.
  Cede a la demanda: 30 frames sin builds de draw real antes de seguir.
- Presupuesto aprendido: la primera sesión usa 16 MiB; al guardar se escribe
  `d3d12_prewarm.bin` (mínimo libre y gasto). La siguiente usa
  `gasto + mínimo libre − 256 MiB`, con un tope de 256 MiB. Si TOTK roza el límite,
  el presupuesto queda cerca de cero: es una capa de admisión segura, no un ahorro
  de memoria directo.
- Prewarm frío: candidatos diferidos con sesión de draw registrada (`LastSession`),
  ordenados por recencia. Un build a la vez en `prewarm_thread`, sin locks durante
  la lectura de disco ni la compilación; el resultado se descarta si el draw real
  ya lo creó.
- `pso_workers=1..8` en boot.cfg sustituye el número de threads de build para el
  ensayo 1/2/4. Sin la línea, el valor por defecto no cambia.
- Prewarm del JIT sin cambios a propósito: más presupuesto llenaría antes las
  cachés con tope (los 3 clears de carga de 0.3.10.5).

Qué revisar en los logs: la línea `cold prewarm of N pipelines ... budget X MiB`,
el cierre por `done` o `budget spent`, el número de pausas, el pico y el mínimo
libre frente a 0.3.10.5, y los tirones de pipeline.

El estado siguiente describe la investigación inicial, antes de esa implementación.

Repositorio clonado completo; inspección estática de ambos proyectos y consulta
de fuentes primarias. Los harness existentes `memory-guard.cpp` y
`pipeline-residency.cpp` pasan compilados con MSVC C++20 /O2 como comprobación
de baseline. Runner y ejecutables locales en
`build-uwp/diagnostics/memory-feasibility/`, fuera de control de versiones.
No se ha compilado el emulador, instalado dependencias grandes,
ejecutado un juego, modificado el renderer ni medido ahorro nuevo.

## Fuentes web consultadas

[ms-resources]: https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation
[ms-expected]: https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.expectedappmemoryusagelimit
[ms-budget]: https://learn.microsoft.com/en-us/archive/msdn-magazine/2016/october/universal-windows-platform-working-with-memory-limits-and-task-priorities-in-the-uwp
[residency]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency
[pso-library]: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary
[defrag]: https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/defragmentation.html
[eden-gc]: https://raw.githubusercontent.com/eden-emulator/mirror/master/src/video_core/texture_cache/texture_cache.h
[ryu-texture]: https://raw.githubusercontent.com/Stella-sea/ryujinx-admin/canary/src/Ryujinx.Graphics.Gpu/Image/Texture.cs
[ryu-docs]: https://docs.ryujinx.app/
