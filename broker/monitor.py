import paho.mqtt.client as mqtt
from dotenv import load_dotenv
import os

load_dotenv()
public_ip = os.getenv("IP", "localhost")

def on_connect(client, userdata, flags, rc):
    print(f"✅ Monitor connected with result code {rc}")
    client.subscribe("device/MC60/sms_tx", qos=0)

def on_message(client, userdata, msg):
    print("🔔 New Data Received!")
    print(f"Topic: {msg.topic}")
    print(f"Payload: {msg.payload.decode(errors='replace')}")

def on_disconnect(client, userdata, rc):
    print(f"⚠️ Disconnected rc={rc}")

client = mqtt.Client(client_id="monitor_pc", clean_session=True)
client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

# دیباگ
client.enable_logger()

client.connect(public_ip, 1883, 60)
client.loop_forever()
