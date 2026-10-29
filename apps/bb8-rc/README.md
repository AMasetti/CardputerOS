# BB-8 RC

Control remoto del **BB-8 original de Sphero** (protocolo v1; el Ollie habla lo mismo) por Bluetooth LE.

## Uso
1. Sacá el BB-8 del cargador y sacudilo: anuncia como `BB-xxxx`.
2. Abrí la app, elegí el robot y apretá enter.
3. `;` `.` adelante/atrás (mantenidas), `,` `/` giran el rumbo, `-` `=` velocidad, `1`-`8` color,
   `space` frena, `s` lo duerme. **Fn + `** sale (se desconecta de forma limpia).
4. Calibrar (`c`): prende la luz azul trasera; girá con `,` `/` hasta que apunte hacia vos y enter.

`d` en la primera pantalla abre un modo demo sin robot.

## Cómo funciona
`src/sphero_core.h` arma los paquetes (`FF SOP2 DID CID SEQ DLEN DATA CHK`) y reconoce los UUID
`22bb746f-2bXX-7554-2d6f-726568705327`; no usa el OS, así que `test/host_test.cpp` lo prueba en el host:
`g++ -std=c++17 apps/bb8-rc/test/host_test.cpp -o /tmp/t && /tmp/t`.

## Estado
Armado con documentación pública del protocolo; **sin probar con un BB-8 real**. A verificar en el robot:
el valor de potencia de TX (`0x07`), el comando de luz trasera (`0x21`) y el de fijar rumbo (`0x01`),
y que no haga falta suscribirse a las notificaciones. Los robots v2 (R2-D2, R2-Q5, BB-9E, Mini, BOLT)
usan otro protocolo y no están soportados todavía.
