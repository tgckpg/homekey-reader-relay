"""Sensor platform for HomeKey Reader."""

from __future__ import annotations

import logging

from homeassistant.components import bluetooth
from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_ADDRESS
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN, EVENT_PACKET, SERVICE_UUID

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
	hass: HomeAssistant,
	entry: ConfigEntry,
	async_add_entities: AddEntitiesCallback,
) -> None:
	"""Set up the HomeKey Reader sensor."""
	async_add_entities([HomeKeyReaderUidSensor(hass, entry)])


class HomeKeyReaderUidSensor(SensorEntity):
	"""Last NFC UID received from one HomeKey Reader."""

	_attr_has_entity_name = True
	_attr_name = "Last NFC UID"
	_attr_icon = "mdi:nfc"

	def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
		self.hass = hass
		self._entry = entry
		self._address = entry.data[CONF_ADDRESS]
		self._attr_unique_id = f"{self._address}_last_uid"
		self._attr_native_value = None
		self._attr_available = False
		self._attrs: dict[str, object] = {}

		self._attr_device_info = DeviceInfo(
			identifiers={(DOMAIN, self._address)},
			name=entry.title,
			manufacturer="HomeKey Reader",
			model="ESP32-C3 + PN532",
		)

	@property
	def extra_state_attributes(self) -> dict[str, object]:
		return self._attrs

	async def async_added_to_hass(self) -> None:
		"""Subscribe to Bluetooth advertisements."""

		@callback
		def _packet_seen(
			service_info: bluetooth.BluetoothServiceInfoBleak,
			change: bluetooth.BluetoothChange,
		) -> None:
			payload = service_info.service_data.get(SERVICE_UUID)
			if payload is None:
				return

			if len(payload) not in (7, 10, 13):
				_LOGGER.warning(
					"Invalid HomeKey Reader packet from %s: %s",
					service_info.address,
					payload.hex(),
				)
				return

			version = payload[0]
			seq = int.from_bytes(payload[1:3], "little")
			data = payload[3:]

			if version != 1:
				_LOGGER.warning(
					"Unsupported HomeKey Reader version from %s: %s",
					service_info.address,
					payload.hex(),
				)
				return

			uid = ":".join(f"{byte:02X}" for byte in data)

			self._attr_native_value = uid
			self._attr_available = True
			self._attrs = {
				"address": service_info.address,
				"rssi": service_info.rssi,
				"sequence": seq,
				"protocol_version": version,
				"raw": payload.hex(),
			}
			self.async_write_ha_state()

			self.hass.bus.async_fire(
				EVENT_PACKET,
				{
					"uid": uid,
					**self._attrs,
				},
			)

		@callback
		def _unavailable(_service_info: bluetooth.BluetoothServiceInfoBleak) -> None:
			self._attr_available = False
			self.async_write_ha_state()

		self.async_on_remove(
			bluetooth.async_register_callback(
				self.hass,
				_packet_seen,
				{
					"address": self._address,
					"service_data_uuid": SERVICE_UUID,
					"connectable": False,
				},
				bluetooth.BluetoothScanningMode.ACTIVE,
				replay=bluetooth.BluetoothCallbackReplay.NEWEST_FIRST,
			)
		)
		self.async_on_remove(
			bluetooth.async_track_unavailable(
				self.hass,
				_unavailable,
				self._address,
				connectable=False,
			)
		)
