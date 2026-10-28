# Changelog — Karting

## [0.1.0]
- Estimación del recorrido solo con el IMU (sin GPS): filtro de actitud, rumbo por el giróscopo y velocidad por el acelerómetro, corregida con a_normal = v·ω en las curvas y con velocidad cero al estar quieto.
- Calibración con G0 sentado y quieto (gravedad y sesgo del giróscopo), válida con el aparato en cualquier orientación en el bolsillo.
- El arranque de la carrera se detecta solo al acelerar.
- Las vueltas se detectan solas por repetición del trazado del circuito, sin línea de meta. Las vueltas de carrera (1-99) y la vuelta de reconocimiento se eligen antes de empezar.
- Una parada brusca (choque) no termina la sesión. Se termina con G0 apretado 5 s o enter apretado 3 s.
- Pitidos en el arranque, en cada vuelta y al completar la carrera.
- Graba en `/karting` de la microSD: IMU crudo, vueltas y trayectoria, con archivos nuevos por sesión y flush cada segundo. Nunca formatea ni borra.
- Resultados en pantalla: tabla de vueltas con la mejor y las diferencias, trazado de la mejor vuelta coloreado por velocidad y datos de la sesión.
- Simulador de PC (`sim/`) para probar el estimador y el detector de vueltas.
