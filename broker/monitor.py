import paho.mqtt.client as mqtt
from dotenv import load_dotenv
import os

load_dotenv()
public_ip = os.getenv("IP", "localhost")


def on_connect(client, userdata, flags, rc):
    print(f"✅ Monitor connected with result code {rc}")
    client.subscribe("device/MC60/commands")

def on_message(client, userdata, msg):
    print(f"🔔 New SMS Data Received!")
    print(f"Topic: {msg.topic}")
    print(f"Payload: {msg.payload.decode()}")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(public_ip, 1883, 60)
client.loop_forever()

