import paho.mqtt.client as mqtt
import time
from dotenv import load_dotenv
import os 
load_dotenv()


public_ip = os.getenv("IP", "localhost")


client = mqtt.Client()
client.connect(public_ip, 1883, 60)



test_payload_en = "+989121234567:Hello from MC60!"
client.publish("device/MC60/sms_rx", test_payload_en)
print("📤 Sent English Mock Data")

time.sleep(1)

# test_payload_fa = "+989121234567:0633064406270645"
# client.publish("device/MC60/sms_rx", test_payload_fa)
# print("📤 Sent Persian (Hex) Mock Data")

client.disconnect()