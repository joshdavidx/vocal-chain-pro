VOCALCHAIN - cadena vocal todo en uno (VST3 para FL Studio, Windows)

Orden de la cadena:
Gate > EQ > Autotune > De-esser > Compresor > Saturacion > Delay > Reverb > Limiter > Salida

COMO OBTENER EL PLUGIN (sin programar)
1. Crea una cuenta gratis en github.com.
2. Crea un repositorio nuevo (boton "New"), ponle de nombre VocalChain.
3. En el repositorio: "Add file" > "Upload files". Descomprime este zip y arrastra TODO el
   contenido de la carpeta VocalChain (incluida la carpeta .github) a esa pantalla.
   Luego pulsa "Commit changes".
4. Ve a la pestana "Actions". Se empieza a compilar solo (tarda unos 10 a 15 minutos).
   Si no arranca, entra a "Compilar VocalChain (Windows)" y pulsa "Run workflow".
5. Cuando salga la palomita verde, abre esa ejecucion y abajo, en "Artifacts", descarga
   "VocalChain-Windows".
6. Descomprime, haz clic derecho en Instalar.bat > "Ejecutar como administrador".
7. En FL Studio: Options > Manage plugins > Find installed plugins. Busca "VocalChain".

SI LA COMPILACION FALLA (cruz roja): abre la ejecucion, copia el texto del error y mandamelo.

NOTAS
- Los efectos son de diseno propio (inspirados en un compresor optico, un EQ y un realzador
  de aire), no son copias de Teletronix, Fresh Air, Mac EQ ni Auto-Tune.
- El "denoise" es una compuerta de ruido (Gate), no un reductor espectral como iZotope RX.
- Autotune: elige Tonalidad y Escala. Velocidad 0 ms = efecto robot; 20 a 40 ms = natural.
- El autotune agrega unos 20 ms de retardo; FL Studio no lo compensa solo. Si lo usas al grabar,
  monitorea sin el plugin.
- Es una primera version: ajusta con calma y dime que suena raro.
