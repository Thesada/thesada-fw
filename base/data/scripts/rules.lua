-- thesada-fw — rules.lua
-- Event-driven rules — loaded at boot, hot-reloadable
-- SPDX-License-Identifier: GPL-3.0-only

Log.info("rules.lua loaded")

-- Example: alert on low supply temperature
-- EventBus.subscribe("temperature", function(data)
--   if data.sensor == "house_supply" and data.value < 40 then
--     MQTT.publish("thesada/owb/alert", "Supply temp low: " .. data.value .. "C")
--   end
-- end)
