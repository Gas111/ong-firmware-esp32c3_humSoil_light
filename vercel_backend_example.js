/**
 * API endpoint para actualizar configuración de sensor y publicar a MQTT
 * 
 * Archivo: api/update-sensor-config.js
 * 
 * Instalación de dependencias:
 * npm install mqtt
 * 
 * Endpoint: POST https://ong-controller.vercel.app/api/update-sensor-config
 * 
 * Body JSON:
 * {
 *   "serial": "0x001C",
 *   "id_sensor": 9,
 *   "interval_seconds": 20,
 *   "max_value": 80.0,
 *   "min_value": 20.0,
 *   "state": "active",
 *   "id_user_created": 3,
 *   "created_at": "2025-11-02T15:13:36.296Z",
 *   "id_user_modified": 5,
 *   "modified_at": "2025-11-03T10:30:00.000Z"
 * }
 */

const mqtt = require('mqtt');

export default async function handler(req, res) {
    // Solo aceptar método POST
    if (req.method !== 'POST') {
        return res.status(405).json({ error: 'Method not allowed' });
    }

    try {
        const {
            serial,
            id_sensor,
            interval_seconds,
            max_value,
            min_value,
            state,
            id_user_created,
            created_at,
            id_user_modified,
            modified_at
        } = req.body;

        // Validar campos requeridos
        if (!serial || !id_sensor) {
            return res.status(400).json({ 
                error: 'Campos requeridos: serial, id_sensor' 
            });
        }

        // Validar serial
        const validSerials = ['0x001C', '0x001D'];
        if (!validSerials.includes(serial)) {
            return res.status(400).json({ 
                error: 'Serial inválido. Valores válidos: 0x001C (humedad), 0x001D (luz)' 
            });
        }

        // Construir el mensaje de configuración
        const sensorConfig = {
            id_sensor,
            interval_seconds: interval_seconds || 20,
            max_value: max_value !== undefined ? max_value : null,
            min_value: min_value !== undefined ? min_value : null,
            state: state || 'active',
            id_user_created: id_user_created || null,
            created_at: created_at || new Date().toISOString(),
            id_user_modified: id_user_modified || null,
            modified_at: modified_at || new Date().toISOString()
        };

        // Configuración del cliente MQTT
        const mqttOptions = {
            host: 'broker.hivemq.com',
            port: 1883,
            protocol: 'mqtt',
            clientId: `vercel_backend_${Date.now()}`,
            clean: true,
            reconnectPeriod: 0, // No reconectar (serverless)
            connectTimeout: 10000
        };

        // Crear cliente MQTT
        const client = mqtt.connect(mqttOptions);

        // Promise para manejar la conexión y publicación
        const publishPromise = new Promise((resolve, reject) => {
            // Timeout de 15 segundos
            const timeout = setTimeout(() => {
                client.end();
                reject(new Error('Timeout al conectar con MQTT broker'));
            }, 15000);

            client.on('connect', () => {
                console.log('✓ Conectado al broker MQTT');
                clearTimeout(timeout);

                // Construir el topic dinámicamente basado en el serial
                const topic = `ong/sensor/${serial}/config`;
                const payload = JSON.stringify(sensorConfig);

                console.log(`Publicando en topic: ${topic}`);
                console.log(`Payload: ${payload}`);

                // Publicar el mensaje con QoS 1 (garantía de entrega)
                client.publish(topic, payload, { qos: 1 }, (error) => {
                    if (error) {
                        client.end();
                        reject(error);
                    } else {
                        console.log('✓ Mensaje publicado exitosamente');
                        client.end();
                        resolve({
                            success: true,
                            topic,
                            config: sensorConfig
                        });
                    }
                });
            });

            client.on('error', (error) => {
                clearTimeout(timeout);
                client.end();
                reject(error);
            });
        });

        // Esperar el resultado de la publicación
        const result = await publishPromise;

        // Respuesta exitosa
        return res.status(200).json({
            success: true,
            message: 'Configuración actualizada y publicada a MQTT',
            serial,
            topic: result.topic,
            config: result.config
        });

    } catch (error) {
        console.error('Error al publicar configuración:', error);
        return res.status(500).json({
            error: 'Error al publicar configuración',
            message: error.message
        });
    }
}

/**
 * EJEMPLO DE USO CON CURL:
 * 
 * curl -X POST https://ong-controller.vercel.app/api/update-sensor-config \
 *   -H "Content-Type: application/json" \
 *   -d '{
 *     "serial": "0x001C",
 *     "id_sensor": 9,
 *     "interval_seconds": 30,
 *     "max_value": 80.0,
 *     "min_value": 20.0,
 *     "state": "active",
 *     "id_user_created": 3,
 *     "created_at": "2025-11-02T15:13:36.296Z",
 *     "id_user_modified": 5,
 *     "modified_at": "2025-11-03T10:30:00.000Z"
 *   }'
 * 
 * EJEMPLO DE USO CON FETCH (FRONTEND):
 * 
 * const updateSensorConfig = async (serial, config) => {
 *   const response = await fetch('/api/update-sensor-config', {
 *     method: 'POST',
 *     headers: {
 *       'Content-Type': 'application/json',
 *     },
 *     body: JSON.stringify({
 *       serial,
 *       ...config
 *     }),
 *   });
 *   
 *   return await response.json();
 * };
 * 
 * // Actualizar sensor de humedad
 * updateSensorConfig('0x001C', {
 *   id_sensor: 9,
 *   interval_seconds: 30,
 *   max_value: 80.0,
 *   min_value: 20.0,
 *   state: 'active'
 * });
 * 
 * INTEGRACIÓN CON FRONTEND:
 * 
 * 1. Agregar en package.json del backend:
 *    "dependencies": {
 *      "mqtt": "^5.3.0"
 *    }
 * 
 * 2. Instalar dependencias:
 *    npm install
 * 
 * 3. Desplegar a Vercel:
 *    vercel --prod
 * 
 * 4. Llamar desde el frontend cuando el usuario modifica la configuración
 *    en la interfaz web
 */
