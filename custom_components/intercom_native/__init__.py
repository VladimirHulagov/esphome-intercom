"""Intercom Native integration for Home Assistant.

This integration provides TCP-based audio streaming between browser and ESP32.
Simple mode: Browser ↔ HA ↔ ESP (port 6054)
Full mode: HA detects ESP going to "Outgoing" state and auto-starts bridge

Unlike WebRTC/go2rtc approaches, this uses simple TCP protocols
which are more reliable across NAT/firewall scenarios.
"""

import logging

import voluptuous as vol

from homeassistant.core import HomeAssistant, CoreState, Event, ServiceCall
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform, EVENT_HOMEASSISTANT_STARTED
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.discovery import async_load_platform

from .const import DOMAIN
from .websocket_api import (
    async_register_websocket_api,
    _get_intercom_devices,
    _stop_device_sessions,
    _find_bridge_by_source,
    _sessions,
    _bridges,
    IntercomSession,
    BridgeSession,
)

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

_LOGGER = logging.getLogger(__name__)


async def _resolve_target_device(hass: HomeAssistant, call: ServiceCall) -> dict | None:
    """Resolve a service call target to an intercom device dict.

    Returns the first matching intercom device from the target selector,
    or None if no intercom device matches.
    """
    from homeassistant.helpers import entity_registry as er

    device_ids = set()

    # Extract device_ids from target (HA resolves entity/area targets to device_ids)
    target = call.data.get("target") or {}
    if hasattr(call, "target") and call.target:
        target = call.target

    # Direct device_id targeting
    if "device_id" in target:
        ids = target["device_id"]
        if isinstance(ids, str):
            device_ids.add(ids)
        elif isinstance(ids, list):
            device_ids.update(ids)

    # Entity targeting: resolve entity -> device
    if "entity_id" in target:
        entity_registry = er.async_get(hass)
        eids = target["entity_id"]
        if isinstance(eids, str):
            eids = [eids]
        for eid in eids:
            entry = entity_registry.async_get(eid)
            if entry and entry.device_id:
                device_ids.add(entry.device_id)

    if not device_ids:
        return None

    # Match against known intercom devices
    intercom_devices = await _get_intercom_devices(hass)
    for dev in intercom_devices:
        if dev["device_id"] in device_ids:
            return dev

    return None


async def _async_register_services(hass: HomeAssistant) -> None:
    """Register HA services for intercom control."""

    async def handle_answer(call: ServiceCall) -> None:
        """Answer an incoming intercom call."""
        device = await _resolve_target_device(hass, call)
        if not device:
            _LOGGER.error("No intercom device found for target")
            return

        device_id = device["device_id"]
        host = device["host"]

        # Check if there's a ringing P2P session
        session = _sessions.get(device_id)
        if session:
            await session.answer()
            return

        # Check bridges where this device is the dest (callee)
        for bridge in _bridges.values():
            if bridge.dest_device_id == device_id and bridge._dest_client:
                await bridge._dest_client.send_answer()
                return

        # No session exists. ESP might have called HA directly. Create a new
        # session and answer.
        session = IntercomSession(hass=hass, device_id=device_id, host=host)
        result = await session.answer_esp_call()
        if result == "streaming":
            _sessions[device_id] = session
            _LOGGER.info("Answered ESP call via service: %s", device["name"])
        else:
            _LOGGER.error("Failed to answer call on %s", device["name"])

    async def handle_decline(call: ServiceCall) -> None:
        """Decline an incoming intercom call."""
        device = await _resolve_target_device(hass, call)
        if not device:
            _LOGGER.error("No intercom device found for target")
            return

        stopped = await _stop_device_sessions(device["device_id"])
        _LOGGER.info("Decline via service: %s (stopped=%s)", device["name"], stopped)

    async def handle_hangup(call: ServiceCall) -> None:
        """End an active intercom call."""
        device = await _resolve_target_device(hass, call)
        if not device:
            _LOGGER.error("No intercom device found for target")
            return

        stopped = await _stop_device_sessions(device["device_id"])
        _LOGGER.info("Hangup via service: %s (stopped=%s)", device["name"], stopped)

    async def handle_call(call: ServiceCall) -> None:
        """Start an intercom call. Target is the destination device.

        If 'source' field is provided, creates an ESP-to-ESP bridge.
        Otherwise, creates a P2P session (HA/browser to ESP).
        """
        dest_device = await _resolve_target_device(hass, call)
        if not dest_device:
            _LOGGER.error("No intercom device found for target")
            return

        source_device_id = call.data.get("source")

        if source_device_id:
            # Bridge mode: source -> dest
            intercom_devices = await _get_intercom_devices(hass)
            source_device = next(
                (d for d in intercom_devices if d["device_id"] == source_device_id),
                None,
            )
            if not source_device:
                _LOGGER.error("Source device not found: %s", source_device_id)
                return

            bridge_id = f"{source_device['device_id']}_{dest_device['device_id']}"

            # Stop existing sessions for both devices
            await _stop_device_sessions(source_device["device_id"])
            await _stop_device_sessions(dest_device["device_id"])

            bridge = BridgeSession(
                hass=hass,
                bridge_id=bridge_id,
                source_device_id=source_device["device_id"],
                source_host=source_device["host"],
                source_name=source_device["name"],
                dest_device_id=dest_device["device_id"],
                dest_host=dest_device["host"],
                dest_name=dest_device["name"],
            )
            _bridges[bridge_id] = bridge
            result = await bridge.start()

            if result in ("connected", "ringing"):
                _LOGGER.info(
                    "Bridge call via service: %s -> %s (%s)",
                    source_device["name"], dest_device["name"], result,
                )
            else:
                _bridges.pop(bridge_id, None)
                _LOGGER.error(
                    "Bridge call failed: %s -> %s",
                    source_device["name"], dest_device["name"],
                )
        else:
            # P2P mode: HA to ESP
            device_id = dest_device["device_id"]
            await _stop_device_sessions(device_id)

            session = IntercomSession(
                hass=hass, device_id=device_id, host=dest_device["host"]
            )
            result = await session.start()

            if result in ("streaming", "ringing"):
                _sessions[device_id] = session
                _LOGGER.info(
                    "P2P call via service: -> %s (%s)", dest_device["name"], result
                )
            else:
                _LOGGER.error("P2P call failed: -> %s", dest_device["name"])

    async def handle_forward(call: ServiceCall) -> None:
        """Forward an active or ringing call to another device.

        Target is the source (caller) device. forward_to is the new destination.
        """
        source_device = await _resolve_target_device(hass, call)
        if not source_device:
            _LOGGER.error("No intercom device found for forward source target")
            return

        forward_to_id = call.data.get("forward_to")
        if not forward_to_id:
            _LOGGER.error("forward_to field is required")
            return

        intercom_devices = await _get_intercom_devices(hass)
        dest_device = next(
            (d for d in intercom_devices if d["device_id"] == forward_to_id),
            None,
        )
        if not dest_device:
            _LOGGER.error("Forward destination device not found: %s", forward_to_id)
            return

        # Find active bridge with this source
        bridge = _find_bridge_by_source(source_device["device_id"])

        if bridge:
            # Forward existing bridge
            result = await bridge.forward_to(
                dest_device["device_id"],
                dest_device["host"],
                dest_device["name"],
            )
            _LOGGER.info(
                "Forward via service: %s -> %s (%s)",
                source_device["name"], dest_device["name"], result,
            )
        else:
            # No active bridge. Create one (source -> new dest).
            # This handles the case where the ESP called HA and we want to
            # route it to another ESP.
            bridge_id = f"{source_device['device_id']}_{dest_device['device_id']}"
            await _stop_device_sessions(source_device["device_id"])

            new_bridge = BridgeSession(
                hass=hass,
                bridge_id=bridge_id,
                source_device_id=source_device["device_id"],
                source_host=source_device["host"],
                source_name=source_device["name"],
                dest_device_id=dest_device["device_id"],
                dest_host=dest_device["host"],
                dest_name=dest_device["name"],
            )
            _bridges[bridge_id] = new_bridge
            result = await new_bridge.start()

            if result in ("connected", "ringing"):
                _LOGGER.info(
                    "Forward (new bridge) via service: %s -> %s (%s)",
                    source_device["name"], dest_device["name"], result,
                )
            else:
                _bridges.pop(bridge_id, None)
                _LOGGER.error(
                    "Forward failed: %s -> %s",
                    source_device["name"], dest_device["name"],
                )

    hass.services.async_register(DOMAIN, "answer", handle_answer)
    hass.services.async_register(DOMAIN, "decline", handle_decline)
    hass.services.async_register(DOMAIN, "hangup", handle_hangup)
    hass.services.async_register(DOMAIN, "call", handle_call)
    hass.services.async_register(DOMAIN, "forward", handle_forward)


async def _async_setup_shared(hass: HomeAssistant, config: dict | None = None) -> None:
    """Shared setup logic for both YAML and config entry."""
    if hass.data.get(DOMAIN, {}).get("initialized"):
        return  # Already set up (e.g. YAML + config entry both present)

    hass.data.setdefault(DOMAIN, {})
    hass.data[DOMAIN]["initialized"] = True

    # Register WebSocket API commands
    async_register_websocket_api(hass)

    # Register HA services (answer, decline, hangup, call)
    await _async_register_services(hass)

    # Load sensor platform
    hass.async_create_task(
        async_load_platform(hass, Platform.SENSOR, DOMAIN, {}, config or {})
    )

    # Register frontend (Lovelace card auto-served from integration)
    async def _register_frontend(_event: Event | None = None) -> None:
        from .frontend import JSModuleRegistration
        registration = JSModuleRegistration(hass)
        await registration.async_register()

    if hass.state == CoreState.running:
        await _register_frontend(None)
    else:
        hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STARTED, _register_frontend)

    _LOGGER.info("Intercom Native integration loaded (simple + full mode auto-bridge)")


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """Set up Intercom Native from configuration.yaml (legacy support)."""
    await _async_setup_shared(hass, config)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up Intercom Native from a config entry (UI setup)."""
    await _async_setup_shared(hass)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True
