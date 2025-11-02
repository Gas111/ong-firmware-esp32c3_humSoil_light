#!/usr/bin/env python3
import serial
import time

def monitor_esp32():
    try:
        # Open serial connection
        ser = serial.Serial('COM7', 115200, timeout=1)
        print("=== ESP32-C3 ADC Monitor ===")
        print("Conectado a COM7 a 115200 baud")
        print("Presiona Ctrl+C para salir")
        print("=" * 40)
        
        while True:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(line)
            time.sleep(0.1)
            
    except serial.SerialException as e:
        print(f"Error al abrir el puerto serial: {e}")
    except KeyboardInterrupt:
        print("\nMonitor detenido por el usuario")
    finally:
        if 'ser' in locals():
            ser.close()
            print("Puerto serial cerrado")

if __name__ == "__main__":
    monitor_esp32()