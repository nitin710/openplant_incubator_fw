# Raspberry Pi gateway setup

Sets up Mosquitto + Node-RED + FlowFuse Dashboard 2.0 on a fresh Raspberry Pi OS install and loads the OpenPlant flow.

## 1. Install

```sh
sudo apt update && sudo apt install -y mosquitto mosquitto-clients

# Let incubators on the LAN connect (Mosquitto 2.x only listens on localhost by default)
printf 'listener 1883\nallow_anonymous true\n' | sudo tee /etc/mosquitto/conf.d/openplant.conf
sudo systemctl enable --now mosquitto && sudo systemctl restart mosquitto

# Node-RED + Node.js (official installer), then start on boot
bash <(curl -sL https://github.com/node-red/linux-installers/releases/latest/download/update-nodejs-and-nodered-deb)
sudo systemctl enable --now nodered

# Dashboard 2.0
cd ~/.node-red && npm install @flowfuse/node-red-dashboard && sudo systemctl restart nodered
```

Give the Pi a fixed IP (DHCP reservation on the router). The incubators connect to `MQTT_HOST` in `incubtaorPwmControl/secrets.h`, so the two must match.

## 2. Import the flow

Open `http://<pi-ip>:1880`, then **☰ → Import**.

1. **Fresh install only:** the flow uses shared config nodes it doesn't contain. Import these first:
   ```json
   [{"id":"broker-pi","type":"mqtt-broker","name":"Pi Mosquitto","broker":"localhost","port":"1883","clientid":"nodered-openplant","autoConnect":true,"usetls":false,"protocolVersion":"4","keepalive":"60","cleansession":true},
    {"id":"ui-base-1","type":"ui-base","name":"Incubator Dashboard","path":"/dashboard","includeClientData":true,"acceptsClientConfig":["ui-notification","ui-control"],"showPathInSidebar":false},
    {"id":"ui-theme-1","type":"ui-theme","name":"Default Theme","colors":{"surface":"#ffffff","primary":"#0094CE","bgPage":"#eeeeee","groupBg":"#ffffff","groupOutline":"#cccccc"},"sizes":{"density":"default","pagePadding":"12px","groupGap":"12px","groupBorderRadius":"4px","widgetGap":"12px"}}]
   ```
2. Import `openplant_dashboard_flow.json` (**select a file to import**).
3. Double-click the **OpenPlant Incubators** tab and set `LOG_DIR` to a folder in your home, e.g. `/home/<user>/openplant`.
4. Click **Deploy**. The MQTT nodes should show a green **connected**.

Dashboard: `http://<pi-ip>:1880/dashboard/incubators`

## 3. Tests

Run these on the Pi.

```sh
# Services are running
systemctl is-active mosquitto nodered            # → active, active

# Ports are listening (1883 = MQTT, 1880 = Node-RED)
ss -ltn | grep -E ':1883|:1880'

# Broker round trip
mosquitto_sub -h localhost -t test/ping -C 1 -W 5 & sleep 1; mosquitto_pub -h localhost -t test/ping -m ok
                                                  # → ok

# Dashboard is served
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:1880/dashboard/incubators   # → 200
```

Fake an incubator to test the flow without hardware:

```sh
T=openplant/incubator/inc-test01
mosquitto_pub -h localhost -r -t $T/status -m online
mosquitto_pub -h localhost -t $T/state -m '{"temp":22.5,"humidity":55,"sensor":"AHT","fan1_rpm":1200,"fan2_rpm":1180,"light":64,"setpoint":22,"cooling":false,"rssi":-60,"uptime":100}'
```

- `inc-test01` appears in the dashboard dropdown with these values.
- A CSV appears: `ls $LOG_DIR` (the folder from step 2.3).
- Moving the set point slider publishes a command; check with
  `mosquitto_sub -h localhost -t "$T/cmd/#" -v`.

Clean up afterwards: `mosquitto_pub -h localhost -r -t $T/status -n` and delete the test CSV.

Real incubators: they should report within 30 s of booting.

```sh
mosquitto_sub -h localhost -t 'openplant/incubator/#' -v    # status, meta, then state every 30 s
```
