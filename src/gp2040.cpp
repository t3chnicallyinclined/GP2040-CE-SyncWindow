// GP2040 includes
#include "gp2040.h"
#include "helper.h"
#include "system.h"
#include "enums.pb.h"

#include "build_info.h"
#include "peripheralmanager.h"
#include "storagemanager.h"
#include "addonmanager.h"
#include "types.h"
#include "usbhostmanager.h"
#include "drivers/dreamcast/DreamcastDriver.h"

// Inputs for Core0
#include "addons/analog.h"
#include "addons/bootsel_button.h"
#include "addons/focus_mode.h"
#include "addons/dualdirectional.h"
#include "addons/tilt.h"
#include "addons/keyboard_host.h"
#include "addons/i2canalog1219.h"
#include "addons/reverse.h"
#include "addons/turbo.h"
#include "addons/slider_socd.h"
#include "addons/spi_analog_ads1256.h"
#include "addons/wiiext.h"
#include "addons/input_macro.h"
#include "addons/snes_input.h"
#include "addons/rotaryencoder.h"
#include "addons/i2c_gpio_pcf8575.h"
#include "addons/gamepad_usb_host.h"
#include "addons/he_trigger.h"
#include "addons/tg16_input.h"

// Pico includes
#include "pico/bootrom.h"
#include "pico/time.h"
#include "hardware/adc.h"

#include "rndis.h"

// TinyUSB
#include "tusb.h"

// USB Input Class Drivers
#include "drivermanager.h"

static const uint32_t REBOOT_HOTKEY_ACTIVATION_TIME_MS = 50;
static const uint32_t REBOOT_HOTKEY_HOLD_TIME_MS = 4000;

const static uint32_t rebootDelayMs = 500;
static absolute_time_t rebootDelayTimeout = nil_time;

void GP2040::setup() {
	Storage::getInstance().init();

	// Reduce CPU if USB host is enabled
	PeripheralManager::getInstance().initUSB();
	if ( PeripheralManager::getInstance().isUSBEnabled(0) ) {
		set_sys_clock_khz(120000, true); // Set Clock to 120MHz to avoid potential USB timing issues
	}

	// I2C & SPI rely on the system clock
	PeripheralManager::getInstance().initSPI();
	PeripheralManager::getInstance().initI2C();

	Gamepad * gamepad = new Gamepad();
	Gamepad * processedGamepad = new Gamepad();
	Storage::getInstance().SetGamepad(gamepad);
	Storage::getInstance().SetProcessedGamepad(processedGamepad);

	// Set pin mappings for all GPIO functions
	Storage::getInstance().setFunctionalPinMappings();

	// power up...
	gamepad->auxState.power.pluggedIn = true;
	gamepad->auxState.power.charging = false;
	gamepad->auxState.power.level = GAMEPAD_AUX_MAX_POWER;

	// Setup Gamepad
	gamepad->setup();

	// Initialize last reinit profile to current so we don't reinit on first loop
	gamepad->lastReinitProfileNumber = Storage::getInstance().getGamepadOptions().profileNumber;

	// now we can load the latest configured profile, which will map the
	// new set of GPIOs to use...
	this->initializeStandardGpio();

	const GamepadOptions& gamepadOptions = Storage::getInstance().getGamepadOptions();

	// check setup options and add modes to the list
	// user modes
	bootActions.insert({GAMEPAD_MASK_B1, gamepadOptions.inputModeB1});
	bootActions.insert({GAMEPAD_MASK_B2, gamepadOptions.inputModeB2});
	bootActions.insert({GAMEPAD_MASK_B3, gamepadOptions.inputModeB3});
	bootActions.insert({GAMEPAD_MASK_B4, gamepadOptions.inputModeB4});
	bootActions.insert({GAMEPAD_MASK_L1, gamepadOptions.inputModeL1});
	bootActions.insert({GAMEPAD_MASK_L2, gamepadOptions.inputModeL2});
	bootActions.insert({GAMEPAD_MASK_R1, gamepadOptions.inputModeR1});
	bootActions.insert({GAMEPAD_MASK_R2, gamepadOptions.inputModeR2});

	// Initialize our ADC (various add-ons)
	adc_init();

	// Setup Add-ons
	addons.LoadUSBAddon(new KeyboardHostAddon());
	addons.LoadUSBAddon(new GamepadUSBHostAddon());
	addons.LoadAddon(new AnalogInput());
	addons.LoadAddon(new HETriggerAddon());
	addons.LoadAddon(new BootselButtonAddon());
	addons.LoadAddon(new DualDirectionalInput());
	addons.LoadAddon(new FocusModeAddon());
	addons.LoadAddon(new I2CAnalog1219Input());
	addons.LoadAddon(new SPIAnalog1256Input());
	addons.LoadAddon(new WiiExtensionInput());
	addons.LoadAddon(new SNESpadInput());
	addons.LoadAddon(new SliderSOCDInput());
	addons.LoadAddon(new TiltInput());
	addons.LoadAddon(new RotaryEncoderInput());
	addons.LoadAddon(new PCF8575Addon());
	addons.LoadAddon(new TG16padInput());

	// Input override addons
	addons.LoadAddon(new ReverseInput());
	addons.LoadAddon(new TurboInput()); // Turbo overrides button states and should be close to the end
	addons.LoadAddon(new InputMacro());

	InputMode inputMode = gamepad->getOptions().inputMode;

	const BootAction bootAction = getBootAction();
	switch (bootAction) {
		case BootAction::ENTER_WEBCONFIG_MODE:
			inputMode = INPUT_MODE_CONFIG;
			break;
		case BootAction::ENTER_USB_MODE:
			reset_usb_boot(0, 0);
			return;
		case BootAction::SET_INPUT_MODE_SWITCH:
			inputMode = INPUT_MODE_SWITCH;
			break;
		case BootAction::SET_INPUT_MODE_KEYBOARD:
			inputMode = INPUT_MODE_KEYBOARD;
			break;
		case BootAction::SET_INPUT_MODE_GENERIC:
			inputMode = INPUT_MODE_GENERIC;
			break;
		case BootAction::SET_INPUT_MODE_NEOGEO:
			inputMode = INPUT_MODE_NEOGEO;
			break;
		case BootAction::SET_INPUT_MODE_MDMINI:
			inputMode = INPUT_MODE_MDMINI;
			break;
		case BootAction::SET_INPUT_MODE_PCEMINI:
			inputMode = INPUT_MODE_PCEMINI;
			break;
		case BootAction::SET_INPUT_MODE_EGRET:
			inputMode = INPUT_MODE_EGRET;
			break;
		case BootAction::SET_INPUT_MODE_ASTRO:
			inputMode = INPUT_MODE_ASTRO;
			break;
		case BootAction::SET_INPUT_MODE_PSCLASSIC:
			inputMode = INPUT_MODE_PSCLASSIC;
			break;
		case BootAction::SET_INPUT_MODE_XINPUT: // X-Input Driver
			inputMode = INPUT_MODE_XINPUT;
			break;
		case BootAction::SET_INPUT_MODE_PS3: // PS3 (HID with quirks) driver
			inputMode = INPUT_MODE_PS3;
			break;
		case BootAction::SET_INPUT_MODE_PS4: // PS4 / PS5 Driver
			inputMode = INPUT_MODE_PS4;
			break;
		case BootAction::SET_INPUT_MODE_PS5: // PS4 / PS5 Driver
			inputMode = INPUT_MODE_PS5;
			break;
		case BootAction::SET_INPUT_MODE_XBONE: // Xbox One Driver
			inputMode = INPUT_MODE_XBONE;
			break;
		case BootAction::SET_INPUT_MODE_XBOXORIGINAL: // Xbox OG Driver
			inputMode = INPUT_MODE_XBOXORIGINAL;
			break;
		case BootAction::SET_INPUT_MODE_SWITCH_PRO:
			inputMode = INPUT_MODE_SWITCH_PRO;
			break;
		case BootAction::SET_INPUT_MODE_DREAMCAST:
			inputMode = INPUT_MODE_DREAMCAST;
			break;
		case BootAction::NONE:
		default:
			break;
	}

	// Always register the mode with DriverManager (for display, LEDs, etc.)
	DriverManager::getInstance().setup(inputMode);

	// save to match user expectations on choosing mode at boot, and this is
	// before USB host will be used so we can force it to ignore the check
	if (inputMode != INPUT_MODE_CONFIG && inputMode != gamepad->getOptions().inputMode) {
		gamepad->setInputMode(inputMode);
		Storage::getInstance().save(true);
	}

	// register system event handlers
	EventManager::getInstance().registerEventHandler(GP_EVENT_STORAGE_SAVE, GPEVENT_CALLBACK(this->handleStorageSave(event)));
	EventManager::getInstance().registerEventHandler(GP_EVENT_RESTART, GPEVENT_CALLBACK(this->handleSystemReboot(event)));
}

/**
 * @brief Initialize standard input button GPIOs that are present in the currently loaded profile.
 */
void GP2040::initializeStandardGpio() {
	GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
	buttonGpios = 0;

	// Reserve Dreamcast Maple Bus pins so they aren't configured as
	// button inputs when DC mode is active (PIO owns them).
	// Uses stored inputMode since this runs before DriverManager::setup().
	// Boot action override to DC mode will re-init GPIO via getReinitGamepad().
	uint32_t reservedPins = 0;
	const GamepadOptions& gpOpts = Storage::getInstance().getGamepadOptions();
	if (gpOpts.inputMode == INPUT_MODE_DREAMCAST &&
	    gpOpts.dreamcastPinA < NUM_BANK0_GPIOS &&
	    gpOpts.dreamcastPinB < NUM_BANK0_GPIOS) {
		reservedPins |= (1u << gpOpts.dreamcastPinA) | (1u << gpOpts.dreamcastPinB);
		if (gpOpts.dreamcastP2PinA < NUM_BANK0_GPIOS)
			reservedPins |= (1u << gpOpts.dreamcastP2PinA);
		if (gpOpts.dreamcastP2PinB < NUM_BANK0_GPIOS)
			reservedPins |= (1u << gpOpts.dreamcastP2PinB);
		if (gpOpts.dreamcastUartRxPin < NUM_BANK0_GPIOS)
			reservedPins |= (1u << gpOpts.dreamcastUartRxPin);
	}

	for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
	{
		if (reservedPins & (1u << pin)) continue;
		// (NONE=-10, RESERVED=-5, ASSIGNED_TO_ADDON=0, everything else is ours)
		if (pinMappings[pin].action > 0)
		{
			gpio_init(pin);             // Initialize pin
			gpio_set_dir(pin, GPIO_IN); // Set as INPUT
			gpio_pull_up(pin);          // Set as PULLUP
			buttonGpios |= 1 << pin;    // mark this pin as mattering for GPIO debouncing
		}
	}
}

/**
 * @brief Deinitialize standard input button GPIOs that are present in the currently loaded profile.
 */
void GP2040::deinitializeStandardGpio() {
	GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
	for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
	{
		// (NONE=-10, RESERVED=-5, ASSIGNED_TO_ADDON=0, everything else is ours)
		if (pinMappings[pin].action > 0)
		{
			gpio_deinit(pin);
		}
	}
}

/**
 * @brief Stock GP2040-CE per-pin debounce.
 *
 * For GPIO that are assigned to buttons (based on GpioMappings, see GP2040::initializeStandardGpio),
 * we can centralize their debouncing here and provide access to it to button users.
 *
 * For ease of use this provides the mask bitwise NOTed so that callers don't have to. To avoid misuse
 * and to simplify this method, non-button GPIO IS NOT PRESENT in this result. Use gpio_get_all directly
 * instead, if you don't want debounced data.
 */
void GP2040::debounceGpioGetAll() {
	Mask_t raw_gpio = ~gpio_get_all();
	Gamepad* gamepad = Storage::getInstance().GetGamepad();

	uint32_t debounceDelay = Storage::getInstance().getGamepadOptions().debounceDelay;
	// Raw passthrough if no delay configured
	if (debounceDelay == 0) {
		gamepad->debouncedGpio = raw_gpio;
		return;
	}

	uint32_t now = getMillis();
	for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++) {
		Mask_t pin_mask = 1 << pin;
		if (buttonGpios & pin_mask) {
			if ((gamepad->debouncedGpio & pin_mask) != \
					(raw_gpio & pin_mask) && ((now - gpioDebounceTime[pin]) > debounceDelay)) {
				gamepad->debouncedGpio ^= pin_mask;
				gpioDebounceTime[pin] = now;
			}
		}
	}
}

void GP2040::syncGpioGetAll() {
	Mask_t raw_gpio = ~gpio_get_all();
	Gamepad* gamepad = Storage::getInstance().GetGamepad();

	const GamepadOptions& options = Storage::getInstance().getGamepadOptions();
	uint32_t nobdSyncDelay = options.nobdSyncDelay;
	bool releaseDebounce = options.nobdReleaseDebounce;

	static bool     sync_pending   = false;
	static uint64_t sync_start_us  = 0;
	static Mask_t   sync_new       = 0;

	static Mask_t   pending_release   = 0;
	static uint64_t release_start_us  = 0;

	uint64_t now_us = to_us_since_boot(get_absolute_time());
	uint64_t syncDelay_us = (uint64_t)nobdSyncDelay * 1000;

	if (!sync_pending && !pending_release &&
	    gamepad->debouncedGpio == (raw_gpio & buttonGpios)) return;

	Mask_t raw_buttons   = raw_gpio & buttonGpios;
	Mask_t prev          = gamepad->debouncedGpio;
	Mask_t just_pressed  = raw_buttons & ~prev & ~sync_new;
	Mask_t just_released = prev & ~raw_buttons;

	if (releaseDebounce) {
		if (just_released) {
			pending_release |= just_released;
			if (release_start_us == 0) release_start_us = now_us;
		}
		pending_release &= ~raw_buttons;
		if (pending_release && (now_us - release_start_us) >= syncDelay_us) {
			gamepad->debouncedGpio &= ~pending_release;
			pending_release  = 0;
			release_start_us = 0;
		}
		if (!pending_release) release_start_us = 0;
	} else {
		if (just_released) gamepad->debouncedGpio &= ~just_released;
	}

	sync_new &= raw_buttons;

	if (just_pressed) {
		if (!sync_pending) {
			sync_pending  = true;
			sync_start_us = now_us;
			sync_new      = just_pressed;
		} else {
			sync_new |= just_pressed;
		}
	}

	if (sync_pending && (now_us - sync_start_us) >= syncDelay_us) {
		gamepad->debouncedGpio |= sync_new;
		sync_pending = false;
		sync_new     = 0;
	}
}

void GP2040::run() {
	Gamepad * gamepad = Storage::getInstance().GetGamepad();
	Gamepad * processedGamepad = Storage::getInstance().GetProcessedGamepad();
	GamepadState prevState;

	DreamcastDriver * dcDriver = DriverManager::getInstance().getDCDriver();
	bool dcMode = (dcDriver != nullptr);

	// P2: second Maple Bus + network input (UART or Ethernet)
	if (dcMode) {
		const GamepadOptions& opts = Storage::getInstance().getGamepadOptions();

		// Always try W6100 Ethernet first (auto-detect via SPI version read)
		// If W6100 not present, falls back to UART
		dcDriver->initEthernet(16, 17, 18, 19, 20);

		// Fall back to UART if Ethernet not detected and UART pin is configured
		if (!dcDriver->ethernetInitialized && opts.dreamcastUartRxPin < NUM_BANK0_GPIOS) {
			dcDriver->initUartRx(opts.dreamcastUartRxPin, 1000000);
		}
		if (opts.dreamcastP2PinA < NUM_BANK0_GPIOS &&
		    opts.dreamcastP2PinB < NUM_BANK0_GPIOS) {
			dcDriver->initP2(opts.dreamcastP2PinA, opts.dreamcastP2PinB);
		}
	}

	bool configMode = false;
	GPDriver * inputDriver = nullptr;

	if (!dcMode) {
		configMode = DriverManager::getInstance().isConfigMode();
		inputDriver = DriverManager::getInstance().getDriver();
		tud_init(TUD_OPT_RHPORT);
		USBHostManager::getInstance().start();
		if (configMode) {
			rndis_init();
		}
	}

	while (1) {
		this->getReinitGamepad(gamepad);
		memcpy(&prevState, &gamepad->state, sizeof(GamepadState));

		if (dcMode) {
			// DC polls at 60Hz (16.67ms) — switch bounce (1-5ms) settles
			// long before the next poll. Always use raw passthrough.
			gamepad->debouncedGpio = ~gpio_get_all();
			gamepad->setDpadMode(DPAD_MODE_DIGITAL);
		} else if (Storage::getInstance().getGamepadOptions().nobdSyncDelay > 0) {
			syncGpioGetAll();
		} else {
			debounceGpioGetAll();
		}

		gamepad->read();

		checkRawState(prevState, gamepad->state);

		if (!dcMode) {
			USBHostManager::getInstance().process();
		}

		if (configMode) {
			inputDriver->process(gamepad);
			rebootHotkeys.process(gamepad, configMode);
			checkSaveRebootState();
			continue;
		}

		addons.PreprocessAddons();

		gamepad->hotkey();
		rebootHotkeys.process(gamepad, false);

		gamepad->process();

		addons.ProcessAddons();

		checkProcessedState(processedGamepad->state, gamepad->state);

		memcpy(&processedGamepad->state, &gamepad->state, sizeof(GamepadState));

		if (dcMode) {
			dcDriver->process(gamepad);
			dcDriver->processP2(gamepad);
		} else {
			bool processed = inputDriver->process(gamepad);
			tud_task();
			addons.PostprocessAddons(processed);
		}

		checkSaveRebootState();

		// DC mode: ISR handles all commands. Main loop just keeps
		// lookup table + analog current and does ISR TX cleanup.
		if (dcMode) {
			// P1: physical buttons via lookup table
			dcDriver->updateCmd9FromGpio(gamepad->debouncedGpio);
			dcDriver->updateAnalogFromGamepad(gamepad);

			// Send local P1 state to relay server
			dcDriver->sendLocalState();

			// Receive merged state from server (P1+P2)
			dcDriver->pollNetwork();
		}
	}
}

void GP2040::getReinitGamepad(Gamepad * gamepad) {
	GamepadOptions& gamepadOptions = Storage::getInstance().getGamepadOptions();

	// Check if profile has changed since last reinit
	if (gamepad->lastReinitProfileNumber != gamepadOptions.profileNumber) {
		uint32_t previousProfile = gamepad->lastReinitProfileNumber;
		uint32_t currentProfile = gamepadOptions.profileNumber;

		// deinitialize the ordinary (non-reserved, non-addon) GPIO pins, since
		// we are moving off of them and onto potentially different pin assignments
		// we currently don't support ASSIGNED_TO_ADDON pins being reinitialized,
		// but if they were to be, that'd be the addon's duty, not ours
		this->deinitializeStandardGpio();

		// now we can load the latest configured profile, which will map the
		// new set of GPIOs to use...
		Storage::getInstance().setFunctionalPinMappings();

		// ...and initialize the pins again
		this->initializeStandardGpio();

		// now we can tell the gamepad that the new mappings are in place
		// and ready to use, and the pins are ready, so it should reinitialize itself
		gamepad->reinit();

		// ...and addons on this core, if they implemented reinit (just things
		// with simple GPIO pin usage, at time of writing)
		addons.ReinitializeAddons();

		// Update the last reinit profile
		gamepad->lastReinitProfileNumber = currentProfile;

		// Trigger the profile change event now that reinit is complete
		EventManager::getInstance().triggerEvent(new GPProfileChangeEvent(previousProfile, currentProfile));
	}
}

GP2040::BootAction GP2040::getBootAction() {
	switch (System::takeBootMode()) {
		case System::BootMode::GAMEPAD: return BootAction::NONE;
		case System::BootMode::WEBCONFIG: return BootAction::ENTER_WEBCONFIG_MODE;
		case System::BootMode::USB: return BootAction::ENTER_USB_MODE;
		case System::BootMode::DEFAULT:
			{
				// Determine boot action based on gamepad state during boot
				Gamepad * gamepad = Storage::getInstance().GetGamepad();
				Gamepad * processedGamepad = Storage::getInstance().GetProcessedGamepad();

				debounceGpioGetAll();
				gamepad->read();

				// Pre-Process add-ons for MPGS
				addons.PreprocessAddons();

				gamepad->process(); // process through MPGS

				// Process for add-ons
				addons.ProcessAddons();

				// Copy Processed Gamepad for Core1 (race condition otherwise)
				memcpy(&processedGamepad->state, &gamepad->state, sizeof(GamepadState));

                const ForcedSetupOptions& forcedSetupOptions = Storage::getInstance().getForcedSetupOptions();
                bool modeSwitchLocked = forcedSetupOptions.mode == FORCED_SETUP_MODE_LOCK_MODE_SWITCH ||
                                        forcedSetupOptions.mode == FORCED_SETUP_MODE_LOCK_BOTH;

                bool webConfigLocked  = forcedSetupOptions.mode == FORCED_SETUP_MODE_LOCK_WEB_CONFIG ||
                                        forcedSetupOptions.mode == FORCED_SETUP_MODE_LOCK_BOTH;

				if (gamepad->pressedS1() && gamepad->pressedS2() && gamepad->pressedUp()) {
					return BootAction::ENTER_USB_MODE;
				} else if (!webConfigLocked && gamepad->pressedS2()) {
					return BootAction::ENTER_WEBCONFIG_MODE;
                } else {
                    if (!modeSwitchLocked) {
                        if (auto search = bootActions.find(gamepad->state.buttons); search != bootActions.end()) {
                            switch (search->second) {
                                case INPUT_MODE_XINPUT:
                                    return BootAction::SET_INPUT_MODE_XINPUT;
                                case INPUT_MODE_SWITCH:
                                    return BootAction::SET_INPUT_MODE_SWITCH;
                                case INPUT_MODE_KEYBOARD:
                                    return BootAction::SET_INPUT_MODE_KEYBOARD;
                                case INPUT_MODE_GENERIC:
                                    return BootAction::SET_INPUT_MODE_GENERIC;
                                case INPUT_MODE_PS3:
                                    return BootAction::SET_INPUT_MODE_PS3;
                                case INPUT_MODE_PS4:
                                    return BootAction::SET_INPUT_MODE_PS4;
                                case INPUT_MODE_PS5:
                                    return BootAction::SET_INPUT_MODE_PS5;
                                case INPUT_MODE_NEOGEO:
                                    return BootAction::SET_INPUT_MODE_NEOGEO;
                                case INPUT_MODE_MDMINI:
                                    return BootAction::SET_INPUT_MODE_MDMINI;
                                case INPUT_MODE_PCEMINI:
                                    return BootAction::SET_INPUT_MODE_PCEMINI;
                                case INPUT_MODE_EGRET:
                                    return BootAction::SET_INPUT_MODE_EGRET;
                                case INPUT_MODE_ASTRO:
                                    return BootAction::SET_INPUT_MODE_ASTRO;
                                case INPUT_MODE_PSCLASSIC:
                                    return BootAction::SET_INPUT_MODE_PSCLASSIC;
                                case INPUT_MODE_XBOXORIGINAL:
                                    return BootAction::SET_INPUT_MODE_XBOXORIGINAL;
                                case INPUT_MODE_XBONE:
                                    return BootAction::SET_INPUT_MODE_XBONE;
                                case INPUT_MODE_SWITCH_PRO:
                                    return BootAction::SET_INPUT_MODE_SWITCH_PRO;
                                case INPUT_MODE_DREAMCAST:
                                    return BootAction::SET_INPUT_MODE_DREAMCAST;
                                default:
                                    return BootAction::NONE;
                            }
                        }
                    }
                }

				break;
			}
	}

	return BootAction::NONE;
}

GP2040::RebootHotkeys::RebootHotkeys() :
	active(false),
	noButtonsPressedTimeout(nil_time),
	webConfigHotkeyMask(GAMEPAD_MASK_S2 | GAMEPAD_MASK_B3 | GAMEPAD_MASK_B4),
	bootselHotkeyMask(GAMEPAD_MASK_S1 | GAMEPAD_MASK_B3 | GAMEPAD_MASK_B4),
	rebootHotkeysHoldTimeout(nil_time) {
}

void GP2040::RebootHotkeys::process(Gamepad* gamepad, bool configMode) {
	// We only allow the hotkey to trigger after we observed no buttons pressed for a certain period of time.
	// We do this to avoid detecting buttons that are held during the boot process. In particular we want to avoid
	// oscillating between webconfig and default mode when the user keeps holding the hotkey buttons.
	if (!active) {
		if (gamepad->state.buttons == 0) {
			if (is_nil_time(noButtonsPressedTimeout)) {
				noButtonsPressedTimeout = make_timeout_time_us(REBOOT_HOTKEY_ACTIVATION_TIME_MS);
			}

			if (time_reached(noButtonsPressedTimeout)) {
				active = true;
			}
		} else {
			noButtonsPressedTimeout = nil_time;
		}
	} else {
		if (gamepad->state.buttons == webConfigHotkeyMask || gamepad->state.buttons == bootselHotkeyMask) {
			if (is_nil_time(rebootHotkeysHoldTimeout)) {
				rebootHotkeysHoldTimeout = make_timeout_time_ms(REBOOT_HOTKEY_HOLD_TIME_MS);
			}

			if (time_reached(rebootHotkeysHoldTimeout)) {
				if (gamepad->state.buttons == webConfigHotkeyMask) {
					// If we are in webconfig mode we go to gamepad mode and vice versa
					System::reboot(configMode ? System::BootMode::GAMEPAD : System::BootMode::WEBCONFIG);
				} else if (gamepad->state.buttons == bootselHotkeyMask) {
					System::reboot(System::BootMode::USB);
				}
			}
		} else {
			rebootHotkeysHoldTimeout = nil_time;
		}
	}
}

void GP2040::checkRawState(GamepadState prevState, GamepadState currState) {
    // buttons pressed
    if (
        ((currState.aux & ~prevState.aux) != 0) ||
        ((currState.dpad & ~prevState.dpad) != 0) ||
        ((currState.buttons & ~prevState.buttons) != 0)
    ) {
        EventManager::getInstance().triggerEvent(new GPButtonDownEvent((currState.dpad & ~prevState.dpad), (currState.buttons & ~prevState.buttons), (currState.aux & ~prevState.aux)));
    }

    // buttons released
    if (
        ((prevState.aux & ~currState.aux) != 0) ||
        ((prevState.dpad & ~currState.dpad) != 0) ||
        ((prevState.buttons & ~currState.buttons) != 0)
    ) {
        EventManager::getInstance().triggerEvent(new GPButtonUpEvent((prevState.dpad & ~currState.dpad), (prevState.buttons & ~currState.buttons), (prevState.aux & ~currState.aux)));
    }
}

void GP2040::checkProcessedState(GamepadState prevState, GamepadState currState) {
    // buttons pressed
    if (
        ((currState.aux & ~prevState.aux) != 0) ||
        ((currState.dpad & ~prevState.dpad) != 0) ||
        ((currState.buttons & ~prevState.buttons) != 0)
    ) {
        EventManager::getInstance().triggerEvent(new GPButtonProcessedDownEvent((currState.dpad & ~prevState.dpad), (currState.buttons & ~prevState.buttons), (currState.aux & ~prevState.aux)));
    }

    // buttons released
    if (
        ((prevState.aux & ~currState.aux) != 0) ||
        ((prevState.dpad & ~currState.dpad) != 0) ||
        ((prevState.buttons & ~currState.buttons) != 0)
    ) {
        EventManager::getInstance().triggerEvent(new GPButtonProcessedUpEvent((prevState.dpad & ~currState.dpad), (prevState.buttons & ~currState.buttons), (prevState.aux & ~currState.aux)));
    }

    if (
        (currState.lx != prevState.lx) ||
        (currState.ly != prevState.ly) ||
        (currState.rx != prevState.rx) ||
        (currState.ry != prevState.ry) ||
        (currState.lt != prevState.lt) ||
        (currState.rt != prevState.rt)
    ) {
        EventManager::getInstance().triggerEvent(new GPAnalogProcessedMoveEvent(currState.lx, currState.ly, currState.rx, currState.ry, currState.lt, currState.rt));
    }
}

void GP2040::checkSaveRebootState() {
	if (saveRequested) {
		saveRequested = false;
		Storage::getInstance().save(forceSave);
	}

	if (rebootRequested) {
		rebootRequested = false;
		rebootDelayTimeout = make_timeout_time_ms(rebootDelayMs);
	}

	if (!is_nil_time(rebootDelayTimeout) && time_reached(rebootDelayTimeout)) {
		System::reboot(rebootMode);
	}
}

void GP2040::handleStorageSave(GPEvent* e) {
	saveRequested = true;
	forceSave = ((GPStorageSaveEvent*)e)->forceSave;
	rebootRequested = ((GPStorageSaveEvent*)e)->restartAfterSave;
	rebootMode = System::BootMode::DEFAULT;
}

void GP2040::handleSystemReboot(GPEvent* e) {
	rebootRequested = true;
	rebootMode = ((GPRestartEvent*)e)->bootMode;
}
