# Fase 0: baseline de memoria y presupuesto de DXIL frío

28 de octubre de 2026. Implementación diagnóstica; gates independientes pasan.
Build UWP, instalación y arranque verificados; cerrada con capturas de TOTK el 30 de octubre (al final).
El diseño general está en [viabilidad de Cemu](xbox_memory_cemu_feasibility.md).

## Qué entrega

La opción existente `memory_audit=1` activa también una línea versionada
`D3D12 memory baseline v1:` en el log del renderer cada cinco segundos de reloj,
si hay frames. Sin esa opción solo se consulta el flag; no se recorre el índice.
No cambia políticas de caché, draws, JIT, prewarm, DRAM ni cuotas.

La línea contiene enteros en bytes y contadores:

- Uso/límite de app, pico muestreado por frame de esta instancia y número de
  muestras válidas; uso/presupuesto GPU por separado.
- Cantidad de pipelines gráficos, compilaciones pendientes, variantes con PSO,
  grupos DXIL únicos, bytes únicos y bytes referenciados contando compartición.
- `cold_upper`: capacidad de grupos DXIL cuyos usuarios observados llevan al
  menos 600 frames de residencia sin Touch. Son frames, no segundos; Touch
  también protege el prewarm. No representa una región del mapa.
- Compute y su DXIL por separado, incluidos split/reduction; ledger de texturas,
  buffers, staging, metadata, bytecode y cantidad global de PSO.
- `scan_us`: tiempo del barrido/agregación; no incluye formato/escritura del log
  ni destrucción de la tabla temporal. No prueba overhead cero.

El barrido se hace en el thread GPU antes de sus Wake/Evict. Se adquiere el
estado publicado por workers y se omiten los pipelines Building, sin esperar
ni leer sus handles/bytecode. Compute mantiene DXIL inmutable desde construcción.
Se agrupa por dirección del objeto DXIL compartido vivo, nunca por hash; no se
persisten direcciones ni se retienen propietarios entre informes.

La tabla temporal admite hasta 65536 grupos (memoria diagnóstica adicional,
liberada después de cada informe). Una observación inconsistente o truncada
invalida `cold_upper`, que pasa a cero con su flag explícito. Se omite también
el barrido con <=8 MiB libres y se captura `bad_alloc` de la tabla diagnóstica,
marcando el informe truncado. Un grupo con una
variante activa no se clasifica como frío aunque otras variantes estén dormidas.

## Qué NO demuestra cold_upper

Es un máximo teórico para los grupos gráficos observados, no bytes liberables
ahora. Pueden existir propietarios en el LRU, claves host de PSO pendientes de
fence, otras variantes en Building o diagnósticos; falta verificar recarga en
disco y diseñar su liberación. No cubre propietarios exclusivamente en esas
rutas. Tampoco convierte bytes GPU en ahorro neto de commit, ni mide tamaño
interno del driver. La medida se toma antes de crear la tabla diagnóstica.

## Comprobaciones reproducibles

Desde la raíz, con MSVC y Python disponibles:

```powershell
.\tools\xbox\tests\run-memory-baseline.ps1
```

El runner también admite `-Python <ruta-a-python.exe>`. Compila el agregador
de producción con C++20 /O2 /W4 /WX y ejecuta el gate Python. Cubre alias activo,
deduplicación, límite frío, Building/fallo, capacidad agotada y observaciones
inconsistentes; parser con presupuesto cambiante, reinicio de caché, límites
desconocidos, entrada inválida y separación de reportes de arranque.

## Build y captura

El usuario autorizó explícitamente compilar e instalar en Xbox. El clon ya tiene
dependencias preparadas y `build-uwp/build.ninja` generado. Comandos del proyecto,
desde `D:\Jadyx`, sujetos a los prerrequisitos de
[compilación UWP](uwp_build.md):

```powershell
.\tools\xbox\build-env.bat cmake --preset uwp-x64
.\tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
```

La compilación integrada UWP y Mesa 26.2.3 finalizaron desde las fuentes y
parches de este clon. El APPX diagnóstico 0.3.9.1 se firmó localmente y verificó
(35 archivos, 739 bloques, firma CMS, metadatos y payload válidos). Se instaló
en Xbox Series S; el usuario confirmó Game y el arranque registró 5120 MiB de
límite. El proceso seguía activo tras aproximadamente un minuto en biblioteca.

Paquete: `build-uwp/package/jadyx.appx`. Símbolos y evidencia de arranque:
`build-uwp/symbols/0.3.9.1`. SHA256 del APPX:
`74C8A0A858C028DF62543262D25C4CCB37E9D26B4811361731D9BF6347303D44`.
El arranque usa `memory_audit=1`, `library=1` y `play=1`; aún no hay captura
de frames de un juego ni evidencia de ahorro de RAM.

El primer configure detectaba mal el target OpenSSL al estar vacío
`CMAKE_SYSTEM_PROCESSOR`. El preset fija `VC-WIN64A-UWP` y pasa `/MD` en
`CMAKE_C_FLAGS`, porque el helper externo no transmite los flags por configuración.
Se comprobó el Makefile generado: familia APP, WindowsApp y `/MD` efectivo.
En este equipo se usó un wrapper local con PATH acotado para que vcvarsall
detectara el SDK. OpenSSL se compila con jom y `CL=/FS` para sincronizar el PDB.

El enlace encontró además símbolos duplicados VirtualAlloc/VirtualProtect en
los wrappers inline del SDK usados por OpenSSL. El parche
`.patch/openssl/0002-uwp-private-memory-wrappers.patch`, registrado en CPM,
les da nombres privados solo en la partición UWP. Conserva las llamadas
VirtualAllocFromApp/VirtualProtectFromApp y el secure heap; el enlace pasó.

El primer build también encontró C2026 de MSVC en el literal de 90 KB de
`network_portal_page.h`. Se dividió en ocho literales adyacentes de hasta 12000
bytes, sin cambiar el contenido servido; `network_portal.cpp` compila después
del cambio. El gate JS reconstruye los fragmentos y controla ese límite.
La sintaxis JS y los vectores SHA pasan. Su mock completo falla en
`renderUpdate` por un elemento sin `classList`; se reprodujo el mismo fallo
con el header y test originales de HEAD, por lo que sigue pendiente aparte.

Solo estaba disponible el certificado público `.cer`, sin clave privada. Para
esta primera instalación se creó una firma local de pruebas `CN=JadyxDev`, con
respaldo PFX cifrado bajo `build-uwp/signing` (ignorado); su contraseña está
protegida por DPAPI para el usuario Windows actual. La clave pública oficial del
actualizador permanece intacta. Este paquete local no debe distribuirse como
release oficial y el actualizador oficial no acepta su firma. La verificación
local debe fijar expresamente la clave del certificado de pruebas.

En la configuración de arranque de la prueba incluir `memory_audit=1` y
`play=1`; mantener juego/update/mods/resolución y cachés de referencia. En PC
conservar el cap 5120 MiB; en Series medir el límite UWP efectivo. Registrar
versión/hash del EXE, hardware, estado de cachés, trayecto y resultado visual.
Las auditorías de heaps preexistentes también se activan: medir overhead total
antes de comparar FPS. No tomar un máximo muestreado como pico absoluto.

En una primera instalación, marcar Jadyx como **Game** en DevHome: seleccionar
Jadyx, botón View, **View details → App type → Game**, y volver a iniciarlo.
El [procedimiento documentado](https://walbourn.github.io/directx-and-uwp-on-xbox-one/)
es por paquete. Microsoft distingue [1 GB para App y 5 GB para Game](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
en Xbox; confirmar siempre `app_limit` real en los logs antes de comparar.

Recorrido manual: cargar partida, permanecer en una zona, girar la cámara,
avanzar y volver, abrir/cerrar mapa y teletransportarse. Registrar las acciones
y sus tiempos para correlacionar; no se ha implementado predictor ni detección
de TP. Cierre manual con Q en la prueba PC directa que lo tenga habilitado.
La primera visita y una repetición con caché caliente son pruebas separadas.

Analizar los archivos exportados de log y diag, sin cargarlos enteros en RAM:

```powershell
python .\tools\xbox\analyze-memory-baseline.py .\jadyx_log.txt .\jadyx_diag.txt --output .\test-results\memory-phase0.json
```

Crear antes `test-results` si no existe. La salida separa cada archivo y cada
instancia detectada por reinicio de frames. Exit 2 significa datos ausentes o
registros baseline inválidos; no un diagnóstico OOM. Los reportes de memoria
antiguos de boot/heartbeat se resumen en MiB, por separado de snapshots en bytes.
No se suman app, GPU y ledger. El pico de arranque y el pico durante frames
permanecen separados; las pausas sin frames pueden escapar al segundo.

Por defecto se calcula déficit respecto al límite más 256 MiB de reserva
experimental (`--reserve-mib` lo cambia). Se conserva el snapshot de mayor
déficit con su propio DXIL frío: no se resta el máximo DXIL de otra escena.
Un presupuesto desconocido no implica memoria libre y no produce déficit cero.
El pico app acumulativo es informativo: no se cruza con un límite de otro momento.

## Cierre de la fase

Compilación integrada, instalación y arranque completados. Falta capturar la
carga problemática en Series y
atribuir los grandes bloques que el ledger aún no explica. Comparar déficit
y conjunto frío, pico durante TP, coste del diagnóstico y recursos que vuelven
a necesitarse. Solo con esa evidencia se decide cuánto puede aportar la fase 1.
Los gates independientes no acreditan ahorro, OOM resuelto ni mejora FPS.

## Actualización integrada: 0.3.9.2

28 de octubre de 2026: base actualizada a `52c95fae38`, conservando el diagnóstico
local y las correcciones de build. Los cambios nuevos añaden recuperación de JIT,
cap de texturas de 768 MiB, mejor empaquetado de heaps y una prueba del ahorro
real al liberar PSO. No implementan conjuntos geográficos por zona.

Configure y build UWP completos (168 tareas). Baseline C++, seis tests Python,
residencia de pipelines y memory-guard pasan. APPX local firmado y verificado:
35 archivos, 743 bloques, mismo EXE y Mesa que el build; entrega única en
`build-uwp/package/jadyx.appx`, símbolos/evidencia en `build-uwp/symbols/0.3.9.2`.
Conserva `memory_audit=1`, `library=1`, `play=1` y la firma local de pruebas.
Esta versión aún no se instaló ni probó con un juego; el arranque Series descrito
arriba corresponde a 0.3.9.1. Ahorro, rendimiento y estabilidad siguen pendientes.
Instalación posterior de 0.3.9.2 completada en Series S (Device Portal Success).
La actualización restableció App type a App; el usuario volvió a marcar Game.
Sesión nueva verificada: 5120 MiB de límite y proceso 0.3.9.2 activo. Evidencia
en symbols/0.3.9.2/boot-xbox-game-diag.txt. Captura de juego todavía pendiente.

## Cierre con datos: 0.3.9.9 a 0.3.10.4 (30 de octubre de 2026)

Capturas de TOTK (0100f2c0115b6000) en Series, App type Game, límite 5120 MiB,
`memory_audit=1`, analizadas con `analyze-memory-baseline.py` (reserva 256 MiB).
Logs en `build-uwp/console-logs/<versión>-totk-*`.

| Versión | Duración | Pico app | Mínimo libre | Déficit | cold_upper | DXIL único / referenciado | Pipelines / PSOs | scan_us máx. |
|---|---|---|---|---|---|---|---|---|
| 0.3.9.9 (OOM) | 26,5 min | 5116 | 8 | 247 | 35 | 86 / 204 | 40686 / 11055 | 9390 |
| 0.3.10.0 | 13,6 min | 5115 | 74 | 181 | 21 | 29 / 370 | 34963 / 3271 | 7613 |
| 0.3.10.1 | 12,6 min | 5040 | 81 | 174 | 24 | 35 / 37 | 3825 / 2476 | 2294 |
| 0.3.10.4 | 5,4 min y salida | 4874 | 271 | 0 | 17 | 27 / 53 | 4675 / 3651 | 1821 |

Valores en MiB salvo conteos y microsegundos.

Conclusiones:

- **El DXIL frío nunca cubrió el déficit.** En el OOM faltaban 247 MiB y el máximo
  teórico recuperable de DXIL era 35. La causa fueron las claves de pipeline que
  variaban por draw (depth bias), los objetos de pipeline, los buffers y la
  metadata del JIT. Se atacó con claves canónicas, pipelines frías hechas al
  dibujarse, GC de buffers bajo presión y compactación de patch sites y perfil JIT.
- **Fase 1 (DXIL con residencia acotada) queda en baja prioridad.** La recarga
  de DXIL ya existe (`dxil_release=1`, 0.3.9.9) y lo que queda frío son 14-24 MiB.
- **Los grandes bloques del heap están atribuidos.** Kernel emulado (~75 MiB), fuentes,
  macros GPU, perfil JIT y las tablas de Dynarmic. Detalle en
  [cache hierarchy](xbox_cache_hierarchy_threads.md).
- **Costo del diagnóstico.** 1,5-1,9 ms cada 5 s con ~4,6k pipelines; con los 40k de
  0.3.9.9 llegaba a 9,4 ms. Hoy no hace falta apagarlo.
- **Margen.** 0.3.10.4 deja 271 MiB libres en la peor muestra, por encima de la
  reserva de 256: el objetivo inicial de la fase se cumple en la captura corta.

Sin verificar: sesión larga (>= 30 min) con 0.3.10.4, Series X (solo se probó la Series S
de 192.168.1.14), recursos que vuelven a necesitarse tras liberarse y
frame p95/p99. El gate final sigue siendo la sesión prolongada sin OOM.
