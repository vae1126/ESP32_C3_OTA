"""
OTA 固件管理平台 — MQTT 客户端（推送升级通知给设备）
"""
import json
import logging
import time
import threading

logger = logging.getLogger("ota-server.mqtt")

mqtt_client = None
_topic_prefix = ""


def init_mqtt(broker, port, username, password, topic_prefix):
    """初始化 MQTT 客户端并连接"""
    global mqtt_client, _topic_prefix
    _topic_prefix = topic_prefix

    try:
        import paho.mqtt.client as mqtt

        mqtt_client = mqtt.Client(client_id="ota-server", protocol=mqtt.MQTTv311)
        mqtt_client.on_connect = _on_connect
        mqtt_client.on_disconnect = _on_disconnect

        if username:
            mqtt_client.username_pw_set(username, password)

        mqtt_client.connect_async(broker, port, 60)
        mqtt_client.loop_start()
        logger.info(f"MQTT connecting to {broker}:{port} ...")
    except ImportError:
        logger.warning("paho-mqtt not installed, MQTT push disabled. Install: pip install paho-mqtt")
    except Exception as e:
        logger.error(f"MQTT connection failed: {e}")


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("MQTT connected to broker")
    else:
        logger.error(f"MQTT connect failed, rc={rc}")


def _on_disconnect(client, userdata, rc):
    logger.warning(f"MQTT disconnected, rc={rc}")


def push_upgrade(device_id, version, action="upgrade"):
    """通过 MQTT 向设备推送升级/降级命令"""
    global mqtt_client
    if not mqtt_client:
        logger.warning("MQTT not available, cannot push command")
        return False

    topic = f"{_topic_prefix}/{device_id}/command"
    payload = json.dumps({
        "action": action,
        "version": version,
        "timestamp": int(time.time())
    })
    result = mqtt_client.publish(topic, payload, qos=1, retain=False)
    logger.info(f"Pushed {action} command to {device_id}: v{version}, rc={result.rc}")
    return result.rc == 0
