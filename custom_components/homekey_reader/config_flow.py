"""Config flow for HomeKey Reader."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.components import bluetooth
from homeassistant.components.bluetooth import BluetoothServiceInfoBleak
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_ADDRESS

from .const import DOMAIN, SERVICE_UUID


class HomeKeyReaderConfigFlow(ConfigFlow, domain=DOMAIN):
	"""Handle a config flow for HomeKey Reader."""

	VERSION = 1

	def __init__(self) -> None:
		self._discovery_info: BluetoothServiceInfoBleak | None = None
		self._discovered: dict[str, BluetoothServiceInfoBleak] = {}

	async def async_step_bluetooth(
		self, discovery_info: BluetoothServiceInfoBleak
	) -> ConfigFlowResult:
		"""Handle Bluetooth discovery."""
		await self.async_set_unique_id(discovery_info.address)
		self._abort_if_unique_id_configured()

		self._discovery_info = discovery_info
		name = discovery_info.name or discovery_info.address
		self.context["title_placeholders"] = {"name": name}
		return await self.async_step_bluetooth_confirm()

	async def async_step_bluetooth_confirm(
		self, user_input: dict[str, Any] | None = None
	) -> ConfigFlowResult:
		"""Confirm a Bluetooth-discovered reader."""
		assert self._discovery_info is not None

		if user_input is not None:
			return self.async_create_entry(
				title=self._discovery_info.name or "HomeKey Reader",
				data={CONF_ADDRESS: self._discovery_info.address},
			)

		self._set_confirm_only()
		return self.async_show_form(
			step_id="bluetooth_confirm",
			description_placeholders={
				"name": self._discovery_info.name or self._discovery_info.address,
				"address": self._discovery_info.address,
			},
		)

	async def async_step_user(
		self, user_input: dict[str, Any] | None = None
	) -> ConfigFlowResult:
		"""Let the user choose a currently visible reader."""
		if user_input is not None:
			address = user_input[CONF_ADDRESS]
			info = self._discovered[address]

			await self.async_set_unique_id(address, raise_on_progress=False)
			self._abort_if_unique_id_configured()

			return self.async_create_entry(
				title=info.name or "HomeKey Reader",
				data={CONF_ADDRESS: address},
			)

		configured = self._async_current_ids(include_ignore=False)
		wanted_uuid = SERVICE_UUID.lower()

		for info in bluetooth.async_discovered_service_info(
			self.hass, connectable=False
		):
			service_data_uuids = {uuid.lower() for uuid in info.service_data}
			if wanted_uuid not in service_data_uuids:
				continue
			if info.address in configured:
				continue
			self._discovered[info.address] = info

		if not self._discovered:
			return self.async_abort(reason="no_devices_found")

		choices = {
			address: f"{info.name or 'HomeKey Reader'} ({address})"
			for address, info in self._discovered.items()
		}

		return self.async_show_form(
			step_id="user",
			data_schema=vol.Schema({vol.Required(CONF_ADDRESS): vol.In(choices)}),
		)
