# esp32-wake-on-lan
Panel en EPS32 para encender equipos por la Red y recibir notificaciones a traves de un bot de telegram

[ Usuario y password default: user: admin password: admin123

¿Qué hace el proyecto?

Este proyecto implementa un panel web de administración en un ESP32 para gestionar Wake on LAN (WOL) y conectividad de red.
El ESP32 actúa como:
•	Cliente Wi-Fi (STA) o Access Point (AP), o ambos simultáneamente
•	Servidor web con autenticación
•	Gestor de múltiples equipos WOL
•	Monitor de estado de equipos mediante ping (ICMP)
•	Emisor de paquetes Wake on LAN (broadcast o dirigido)
•	Emisor opcional de notificaciones vía Telegram
Toda la configuración se realiza desde una interfaz web, sin necesidad de recompilar el firmware.
________________________________________
¿Por qué es útil el proyecto?

Este proyecto es útil cuando se necesita:
•	Encender computadoras o servidores remotamente sin depender de un PC intermedio
•	Centralizar Wake on LAN para múltiples equipos
•	Tener un dispositivo autónomo y de bajo consumo (ESP32)
•	Configurar red, credenciales y WOL dinámicamente
•	Operar incluso en entornos sin infraestructura (modo Access Point)
•	Recibir notificaciones de estado de los equipos
Casos típicos de uso:
•	Home lab
•	Laboratorios de redes
•	Oficinas pequeñas
•	Soporte técnico remoto
•	Automatización básica de encendido de equipos
________________________________________
¿Cómo pueden los usuarios comenzar con el proyecto?

Requisitos
•	ESP32 (probado con DOIT ESP32 DEVKIT V1)


•	Arduino IDE

•	Librerías:

o	WiFi

o	WebServer

o	Preferences

o	WiFiUdp

o	ESPping

o	WiFiClientSecure

o	HTTPClient

Pasos básicos
1.	Clonar o descargar este repositorio
2.	Abrir el archivo .ino en Arduino IDE
3.	Seleccionar la placa ESP32 correcta
4.	Compilar y cargar el firmware
5.	Al iniciar:
o	Si no hay Wi-Fi configurado, el ESP32 levanta un Access Point
6.	Acceder al panel web desde el navegador
7.	Configurar:
o	Wi-Fi (STA)
o	Access Point
o	Usuario y contraseña del panel
o	Equipos WOL
o	Puerto WOL
o	Telegram (opcional)
________________________________________
¿Dónde pueden los usuarios obtener ayuda con el proyecto?

Los usuarios pueden obtener ayuda mediante:
•	La sección Issues del repositorio de GitHub
•	Revisando el código fuente (está comentado y organizado por secciones)
•	Probando primero con configuración mínima (AP + 1 equipo WOL)
Para reportar un problema, es recomendable incluir:
•	Modelo exacto del ESP32
•	Logs del monitor serial
•	Configuración de red utilizada
•	Qué acción produjo el error
________________________________________
¿Quién mantiene y contribuye al proyecto?

Este proyecto es mantenido por Luis Leonel Gómez Álvarez.
Actualmente:
•	El desarrollo y mantenimiento es individual
•	Las contribuciones externas son bienvenidas mediante pull requests
•	Las mejoras se evalúan según estabilidad y coherencia con el diseño original


