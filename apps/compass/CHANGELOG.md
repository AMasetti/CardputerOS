# Changelog — Brújula

## [0.1.0]
- Fusión de acelerómetro y giróscopo (filtro de Mahony) para la orientación en 3D.
- Vista 3D: el Cardputer dibujado sobre una rosa de los vientos, con cámara detrás que sigue el rumbo.
- Vista 2D: dial clásico que gira, rumbo en grados y punto cardinal, nivel de burbuja e inclinación.
- El norte se fija con `enter` (el Cardputer-Adv no tiene magnetómetro) y se ajusta con `,` `/`.
- Calibración del giróscopo al arrancar y con `c`; corrección continua del sesgo mientras está quieto.
- `x` cambia la orientación del sensor por si los ejes no coinciden.
