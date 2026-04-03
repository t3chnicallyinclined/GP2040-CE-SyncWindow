#include "ButtonLayoutScreen.h"
#include "buttonlayouts.h"
#include "drivermanager.h"
#include "drivers/ps4/PS4Driver.h"
#include "drivers/xbone/XBOneDriver.h"
#include "drivers/xinput/XInputDriver.h"
#include "drivers/dreamcast/DreamcastDriver.h"
#include "hardware/timer.h"

void ButtonLayoutScreen::init() {
    isInputHistoryEnabled = Storage::getInstance().getDisplayOptions().inputHistoryEnabled;
    inputHistoryX = Storage::getInstance().getDisplayOptions().inputHistoryRow;
    inputHistoryY = Storage::getInstance().getDisplayOptions().inputHistoryCol;
    inputHistoryLength = Storage::getInstance().getDisplayOptions().inputHistoryLength;
    bannerDelayStart = getMillis();
    gamepad = Storage::getInstance().GetGamepad();
    inputMode = DriverManager::getInstance().getInputMode();

    EventManager::getInstance().registerEventHandler(GP_EVENT_PROFILE_CHANGE, GPEVENT_CALLBACK(this->handleProfileChange(event)));
    EventManager::getInstance().registerEventHandler(GP_EVENT_USBHOST_MOUNT, GPEVENT_CALLBACK(this->handleUSB(event)));
    EventManager::getInstance().registerEventHandler(GP_EVENT_USBHOST_UNMOUNT, GPEVENT_CALLBACK(this->handleUSB(event)));
    
    footer = "";
    historyString = "";
    inputHistory.clear();

    setViewport((isInputHistoryEnabled ? 8 : 0), 0, (isInputHistoryEnabled ? 56 : getRenderer()->getDriver()->getMetrics()->height), getRenderer()->getDriver()->getMetrics()->width);

    uint16_t elementCtr = 0;
    LayoutManager::LayoutList currLayoutLeft = LayoutManager::getInstance().getLayoutA();
    LayoutManager::LayoutList currLayoutRight = LayoutManager::getInstance().getLayoutB();
    for (elementCtr = 0; elementCtr < currLayoutLeft.size(); elementCtr++) {
        pushElement(currLayoutLeft[elementCtr]);
    }
    for (elementCtr = 0; elementCtr < currLayoutRight.size(); elementCtr++) {
        pushElement(currLayoutRight[elementCtr]);
    }

	bannerDisplay = true;
    prevProfileNumber = -1;

    prevLayoutLeft = Storage::getInstance().getDisplayOptions().buttonLayout;
    prevLayoutRight = Storage::getInstance().getDisplayOptions().buttonLayoutRight;
    prevLeftOptions = Storage::getInstance().getDisplayOptions().buttonLayoutCustomOptions.paramsLeft;
    prevRightOptions = Storage::getInstance().getDisplayOptions().buttonLayoutCustomOptions.paramsRight;
    prevOrientation = Storage::getInstance().getDisplayOptions().buttonLayoutOrientation;

    // we cannot look at macro options enabled, pull the pins
    
    // macro display now uses our pin functions, so we need to check if pins are enabled...
    macroEnabled = false;
    hasTurboAssigned = false;
    // Macro Button initialized by void Gamepad::setup()
    GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
    for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
    {
        switch( pinMappings[pin].action ) {
            case GpioAction::BUTTON_PRESS_MACRO:
            case GpioAction::BUTTON_PRESS_MACRO_1:
            case GpioAction::BUTTON_PRESS_MACRO_2:
            case GpioAction::BUTTON_PRESS_MACRO_3:
            case GpioAction::BUTTON_PRESS_MACRO_4:
            case GpioAction::BUTTON_PRESS_MACRO_5:
            case GpioAction::BUTTON_PRESS_MACRO_6:
                macroEnabled = true;
                break;
            case GpioAction::BUTTON_PRESS_TURBO:
                hasTurboAssigned = true;
                break;
            default:
                break;
        }
    }

    // determine which fields will be displayed on the status bar
    showInputMode = Storage::getInstance().getDisplayOptions().inputMode;
    showTurboMode = Storage::getInstance().getDisplayOptions().turboMode && hasTurboAssigned;
    showDpadMode = Storage::getInstance().getDisplayOptions().dpadMode;
    showSocdMode = Storage::getInstance().getDisplayOptions().socdMode;
    showMacroMode = Storage::getInstance().getDisplayOptions().macroMode;
    showProfileMode = Storage::getInstance().getDisplayOptions().profileMode;

    getRenderer()->clearScreen();
}

void ButtonLayoutScreen::shutdown() {
    clearElements();

    EventManager::getInstance().unregisterEventHandler(GP_EVENT_PROFILE_CHANGE, GPEVENT_CALLBACK(this->handleProfileChange(event)));
    EventManager::getInstance().unregisterEventHandler(GP_EVENT_USBHOST_MOUNT, GPEVENT_CALLBACK(this->handleUSB(event)));
    EventManager::getInstance().unregisterEventHandler(GP_EVENT_USBHOST_UNMOUNT, GPEVENT_CALLBACK(this->handleUSB(event)));
}

int8_t ButtonLayoutScreen::update() {
    bool configMode = DriverManager::getInstance().isConfigMode();
    uint8_t profileNumber = getGamepad()->getOptions().profileNumber;
    
    // Check if we've updated button layouts while in config mode
    if (configMode) {
        uint8_t layoutLeft = Storage::getInstance().getDisplayOptions().buttonLayout;
        uint8_t layoutRight = Storage::getInstance().getDisplayOptions().buttonLayoutRight;
        uint8_t buttonLayoutOrientation = Storage::getInstance().getDisplayOptions().buttonLayoutOrientation;
        bool inputHistoryEnabled = Storage::getInstance().getDisplayOptions().inputHistoryEnabled;
        if ((prevLayoutLeft != layoutLeft) || (prevLayoutRight != layoutRight) || (isInputHistoryEnabled != inputHistoryEnabled) || compareCustomLayouts() || (prevOrientation != buttonLayoutOrientation)) {
            shutdown();
            init();
        }
    }

    if (prevProfileNumber != profileNumber) {
        prevProfileNumber = profileNumber;
        bannerDelayStart = getMillis();
        bannerDisplay = true;
    }

	generateHeader();
    if (isInputHistoryEnabled)
		processInputHistory();

    if (DriverManager::getInstance().isConfigMode()) {
        uint16_t buttonState = getGamepad()->state.buttons;
        if (prevButtonState && !buttonState) {
            if (prevButtonState == GAMEPAD_MASK_B1) {
                prevButtonState = 0;
                return DisplayMode::CONFIG_INSTRUCTION;
            }
        }
        prevButtonState = buttonState;
    }

	return -1;
}

void ButtonLayoutScreen::generateHeader() {
	// Limit to 21 chars with 6x8 font for now
	statusBar.clear();
	Storage& storage = Storage::getInstance();

	// Display Profile # banner
	if ( bannerDisplay ) {
		if (((getMillis() - bannerDelayStart) / 1000) < bannerDelay) {
			if (bannerMessage.empty()) {
				statusBar.assign(storage.currentProfileLabel(), strlen(storage.currentProfileLabel()));
				if (statusBar.empty()) {
					statusBar = "     Profile #";
					statusBar +=  std::to_string(getGamepad()->getOptions().profileNumber);
				} else {
					statusBar.insert(statusBar.begin(), (21-statusBar.length())/2, ' ');
				}
			} else {
				statusBar = bannerMessage;
			}
			return;
		} else {
			bannerDisplay = false;
            bannerMessage.clear();
		}
	}

    if (showInputMode) {
        // Display standard header
        switch (inputMode)
        {
            case INPUT_MODE_PS3:    statusBar += "PS3"; break;
            case INPUT_MODE_GENERIC: statusBar += "USBHID"; break;
            case INPUT_MODE_SWITCH: statusBar += "SWITCH"; break;
            case INPUT_MODE_MDMINI: statusBar += "GEN/MD"; break;
            case INPUT_MODE_NEOGEO: statusBar += "NGMINI"; break;
            case INPUT_MODE_PCEMINI: statusBar += "PCE/TG"; break;
            case INPUT_MODE_EGRET: statusBar += "EGRET"; break;
            case INPUT_MODE_ASTRO: statusBar += "ASTRO"; break;
            case INPUT_MODE_PSCLASSIC: statusBar += "PSC"; break;
            case INPUT_MODE_XBOXORIGINAL: statusBar += "OGXBOX"; break;
            case INPUT_MODE_SWITCH_PRO: statusBar += "SWPRO"; break;
            case INPUT_MODE_PS4:
                statusBar += "PS4";
                if(((PS4Driver*)DriverManager::getInstance().getDriver())->getAuthSent() == true )
                    statusBar += ":AS";
                else
                    statusBar += "   ";
                break;
            case INPUT_MODE_PS5:
                statusBar += "PS5";
                if(((PS4Driver*)DriverManager::getInstance().getDriver())->getAuthSent() == true )
                    statusBar += ":AS";
                else
                    statusBar += "   ";
                break;
            case INPUT_MODE_XBONE:
                statusBar += "XBON";
                if(((XBOneDriver*)DriverManager::getInstance().getDriver())->getAuthSent() == true )
                    statusBar += "E";
                else
                    statusBar += "*";
                break;
            case INPUT_MODE_XINPUT:
                statusBar += "X";
                if(((XInputDriver*)DriverManager::getInstance().getDriver())->getAuthSent() == true )
                    statusBar += "B360";
                else
                    statusBar += "INPUT";
                break;
            case INPUT_MODE_KEYBOARD: statusBar += "HID-KB"; break;
            case INPUT_MODE_DREAMCAST: {
                DreamcastDriver* dc = DriverManager::getInstance().getDCDriver();
                if (dc) {
                    static int vmuUsedBlocks = 0;
                    static uint32_t vmuLastFlashCount = 0xFFFFFFFF;
                    if (!dc->disableVMU && dc->vmu.debugVmuFlashCount != vmuLastFlashCount) {
                        vmuLastFlashCount = dc->vmu.debugVmuFlashCount;
                        vmuUsedBlocks = 0;
                        const uint8_t* fatRaw = dc->vmu.getRawPointer() + VMU_FAT_BLOCK_NO * VMU_BYTES_PER_BLOCK;
                        const uint32_t* fatWords = (const uint32_t*)fatRaw;
                        for (int i = 0; i < 100; i++) {
                            uint32_t fw = fatWords[i];
                            uint16_t a = (uint16_t)(((fw >> 8) & 0xFF) << 8 | (fw & 0xFF));
                            uint16_t b = (uint16_t)(((fw >> 24) & 0xFF) << 8 | ((fw >> 16) & 0xFF));
                            if (a != 0xFFFC) vmuUsedBlocks++;
                            if (b != 0xFFFC) vmuUsedBlocks++;
                        }
                    }
                    char dcBuf[24];
                    if (dc->disableVMU) {
                        snprintf(dcBuf, sizeof(dcBuf), "DC VMU:OFF");
                    } else {
                        snprintf(dcBuf, sizeof(dcBuf), "DC VMU:%d/%d", vmuUsedBlocks, 200);
                    }
                    statusBar += dcBuf;
                } else {
                    statusBar += "DC";
                }
                break;
            }
            case INPUT_MODE_CONFIG: statusBar += "CONFIG"; break;
        }
    }

    if (showTurboMode) {
        const TurboOptions& turboOptions = storage.getAddonOptions().turboOptions;
        if ( turboOptions.enabled ) {
            statusBar += " T";
            if ( turboOptions.shotCount < 10 ) // padding
                statusBar += "0";
            statusBar += std::to_string(turboOptions.shotCount);
        } else {
            statusBar += "    "; // no turbo, don't show Txx setting
        }
    }

	const GamepadOptions & options = gamepad->getOptions();

    if (showDpadMode) {
        switch (gamepad->getActiveDpadMode())
        {
            case DPAD_MODE_DIGITAL:      statusBar += " D"; break;
            case DPAD_MODE_LEFT_ANALOG:  statusBar += " L"; break;
            case DPAD_MODE_RIGHT_ANALOG: statusBar += " R"; break;
        }
    }

    if (showSocdMode) {
        switch (Gamepad::resolveSOCDMode(gamepad->getOptions()))
        {
            case SOCD_MODE_NEUTRAL:               statusBar += " SOCD-N"; break;
            case SOCD_MODE_UP_PRIORITY:           statusBar += " SOCD-U"; break;
            case SOCD_MODE_SECOND_INPUT_PRIORITY: statusBar += " SOCD-L"; break;
            case SOCD_MODE_FIRST_INPUT_PRIORITY:  statusBar += " SOCD-F"; break;
            case SOCD_MODE_BYPASS:                statusBar += " SOCD-X"; break;
        }
    }

    if (showMacroMode && macroEnabled) statusBar += " M";

    if (showProfileMode) {
        statusBar += " ";

        std::string profile;
        profile.assign(storage.currentProfileLabel(), strlen(storage.currentProfileLabel()));
        if (profile.empty()) {
            statusBar += std::to_string(getGamepad()->getOptions().profileNumber);
        } else {
            statusBar += profile;
        }
    }

    trim(statusBar);
}

void ButtonLayoutScreen::drawScreen() {
    DreamcastDriver* dcDriver = DriverManager::getInstance().getDCDriver();
    DreamcastDriver* dcDriverP2 = DriverManager::getInstance().getDCDriverP2();

    if (inputMode == INPUT_MODE_DREAMCAST && dcDriver && dcDriver->enableDiagnostics) {
        auto* dc = dcDriver;
        char buf[64];

        getRenderer()->clearScreen();

        if (dc->diagPage == 0) {
            // PAGE 0: MAPLE BUS
            snprintf(buf, sizeof(buf), "=MAPLE= S1:page");
            getRenderer()->drawText(0, 0, std::string(buf));

            {
                uint32_t fMin = (dc->frameIntervalMin == 0xFFFFFFFF) ? 0 : dc->frameIntervalMin;
                snprintf(buf, sizeof(buf), "F:%lu-%luus D:%lu",
                         (unsigned long)fMin, (unsigned long)dc->frameIntervalMax,
                         (unsigned long)dc->droppedPollCount);
            }
            getRenderer()->drawText(0, 1, std::string(buf));

            {
                uint32_t rMin = (dc->respMin == 0xFFFFFFFF) ? 0 : dc->respMin;
                uint32_t bMin = (dc->b2pMin == 0xFFFFFFFF) ? 0 : dc->b2pMin;
                snprintf(buf, sizeof(buf), "ISR:%luus B2P:%lu-%lu",
                         (unsigned long)rMin,
                         (unsigned long)bMin, (unsigned long)dc->b2pMax);
            }
            getRenderer()->drawText(0, 2, std::string(buf));

            snprintf(buf, sizeof(buf), "c9:%lu XF:%lu Rx:%lu",
                     (unsigned long)dc->debugCmd9Count,
                     (unsigned long)dc->debugXorFail,
                     (unsigned long)dc->debugRxCount);
            getRenderer()->drawText(0, 3, std::string(buf));

            snprintf(buf, sizeof(buf), "v14:%lu burst:%lu",
                     (unsigned long)dc->cmd14Count,
                     (unsigned long)dc->cmd14BurstCount);
            getRenderer()->drawText(0, 4, std::string(buf));

            snprintf(buf, sizeof(buf), "VMU ok:%lu er:%lu",
                     (unsigned long)dc->vmu.debugVmuReadOkCount,
                     (unsigned long)dc->vmu.debugVmuReadErrCount);
            getRenderer()->drawText(0, 5, std::string(buf));

        } else if (dc->diagPage == 1) {
            // PAGE 1: P2 NETWORK INPUT (P2 is inside dc driver)
            snprintf(buf, sizeof(buf), "=P2 NET= S1:page");
            getRenderer()->drawText(0, 0, std::string(buf));

            snprintf(buf, sizeof(buf), "rx:%lu app:%lu",
                     (unsigned long)dc->netFrameCount,
                     (unsigned long)dc->netApplyCount);
            getRenderer()->drawText(0, 1, std::string(buf));

            {
                uint32_t iMin = (dc->netIntervalMin == 0xFFFFFFFF) ? 0 : dc->netIntervalMin;
                snprintf(buf, sizeof(buf), "int:%lu-%luus",
                         (unsigned long)iMin,
                         (unsigned long)dc->netIntervalMax);
            }
            getRenderer()->drawText(0, 2, std::string(buf));

            snprintf(buf, sizeof(buf), "last:%luus",
                     (unsigned long)dc->netIntervalLast);
            getRenderer()->drawText(0, 3, std::string(buf));

            snprintf(buf, sizeof(buf), "bad:%lu latch:%s",
                     (unsigned long)dc->netBadSync,
                     dc->hasNetState ? "ON" : "off");
            getRenderer()->drawText(0, 4, std::string(buf));

            snprintf(buf, sizeof(buf), "eth:%s v:%02X p2:%s",
                     dc->ethernetInitialized ? "OK" : "NO",
                     (unsigned)dc->ethernetChipVersion,
                     dc->p2Enabled ? "OK" : "NO");
            getRenderer()->drawText(0, 5, std::string(buf));

        } else {
            // PAGE 2: LIVE STATE — P1 + P2 side by side
            snprintf(buf, sizeof(buf), "=STATE= S1:page");
            getRenderer()->drawText(0, 0, std::string(buf));

            snprintf(buf, sizeof(buf), "P1:%08lX",
                     (unsigned long)dc->cmd9ReadyW3);
            getRenderer()->drawText(0, 1, std::string(buf));

            snprintf(buf, sizeof(buf), "gpio:%08lX",
                     (unsigned long)dc->lastFilteredGpio);
            getRenderer()->drawText(0, 2, std::string(buf));

            snprintf(buf, sizeof(buf), "P2:%08lX",
                     (unsigned long)dc->p2Cmd9ReadyW3);
            getRenderer()->drawText(0, 3, std::string(buf));

            {
                uint32_t age = dc->hasNetState ?
                    (timer_hw->timerawl - dc->lastNetTimestamp) / 1000 : 9999;
                snprintf(buf, sizeof(buf), "net:%08lX %lums",
                         (unsigned long)dc->netLastW3,
                         (unsigned long)age);
            }
            getRenderer()->drawText(0, 4, std::string(buf));

            snprintf(buf, sizeof(buf), "mask:%08lX",
                     (unsigned long)dc->buttonGpioMask);
            getRenderer()->drawText(0, 5, std::string(buf));
        }

        return;
    }

    if (bannerDisplay) {
        getRenderer()->drawRectangle(0, 0, 128, 7, true, true);
    	getRenderer()->drawText(0, 0, statusBar, true);
    } else {
		getRenderer()->drawText(0, 0, statusBar);
	}

    // DC mode: normal button layout shown (ISR handles Maple Bus independently)

    getRenderer()->drawText(0, 7, footer);
}

GPLever* ButtonLayoutScreen::addLever(uint16_t startX, uint16_t startY, uint16_t sizeX, uint16_t sizeY, uint16_t strokeColor, uint16_t fillColor, uint16_t inputType) {
    GPLever* lever = new GPLever();
    lever->setRenderer(getRenderer());
    lever->setPosition(startX, startY);
    lever->setStrokeColor(strokeColor);
    lever->setFillColor(fillColor);
    lever->setRadius(sizeX);
    lever->setInputType(inputType);
    lever->setViewport(this->getViewport());
    return (GPLever*)addElement(lever);
}

GPButton* ButtonLayoutScreen::addButton(uint16_t startX, uint16_t startY, uint16_t sizeX, uint16_t sizeY, uint16_t strokeColor, uint16_t fillColor, int16_t inputMask) {
    GPButton* button = new GPButton();
    button->setRenderer(getRenderer());
    button->setPosition(startX, startY);
    button->setStrokeColor(strokeColor);
    button->setFillColor(fillColor);
    button->setSize(sizeX, sizeY);
    button->setInputMask(inputMask);
    button->setViewport(this->getViewport());
    return (GPButton*)addElement(button);
}

GPShape* ButtonLayoutScreen::addShape(uint16_t startX, uint16_t startY, uint16_t sizeX, uint16_t sizeY, uint16_t strokeColor, uint16_t fillColor) {
    GPShape* shape = new GPShape();
    shape->setRenderer(getRenderer());
    shape->setPosition(startX, startY);
    shape->setStrokeColor(strokeColor);
    shape->setFillColor(fillColor);
    shape->setSize(sizeX,sizeY);
    shape->setViewport(this->getViewport());
    return (GPShape*)addElement(shape);
}

GPSprite* ButtonLayoutScreen::addSprite(uint16_t startX, uint16_t startY, uint16_t sizeX, uint16_t sizeY) {
    GPSprite* sprite = new GPSprite();
    sprite->setRenderer(getRenderer());
    sprite->setPosition(startX, startY);
    sprite->setSize(sizeX,sizeY);
    sprite->setViewport(this->getViewport());
    return (GPSprite*)addElement(sprite);
}

GPWidget* ButtonLayoutScreen::pushElement(GPButtonLayout element) {
    if (element.elementType == GP_ELEMENT_LEVER) {
        return addLever(element.parameters.x1, element.parameters.y1, element.parameters.x2, element.parameters.y2, element.parameters.stroke, element.parameters.fill, element.parameters.value);
    } else if ((element.elementType == GP_ELEMENT_BTN_BUTTON) || (element.elementType == GP_ELEMENT_DIR_BUTTON) || (element.elementType == GP_ELEMENT_PIN_BUTTON)) {
        GPButton* button = addButton(element.parameters.x1, element.parameters.y1, element.parameters.x2, element.parameters.y2, element.parameters.stroke, element.parameters.fill, element.parameters.value);

        // set type of button
        button->setInputType(element.elementType);
        button->setInputDirection(false);
        button->setShape((GPShape_Type)element.parameters.shape);
        button->setAngle(element.parameters.angleStart);
        button->setAngleEnd(element.parameters.angleEnd);
        button->setClosed(element.parameters.closed);

        if (element.elementType == GP_ELEMENT_DIR_BUTTON) button->setInputDirection(true);

        return (GPWidget*)button;
    } else if (element.elementType == GP_ELEMENT_SPRITE) {
        return addSprite(element.parameters.x1, element.parameters.y1, element.parameters.x2, element.parameters.y2);
    } else if (element.elementType == GP_ELEMENT_SHAPE) {
        GPShape* shape = addShape(element.parameters.x1, element.parameters.y1, element.parameters.x2, element.parameters.y2, element.parameters.stroke, element.parameters.fill);
        shape->setShape((GPShape_Type)element.parameters.shape);
        shape->setAngle(element.parameters.angleStart);
        shape->setAngleEnd(element.parameters.angleEnd);
        shape->setClosed(element.parameters.closed);
        return shape;
    }
    return NULL;
}

void ButtonLayoutScreen::processInputHistory() {
	std::deque<std::string> pressed;

	// Get key states
	std::array<bool, INPUT_HISTORY_MAX_INPUTS> currentInput = {

		pressedUp(),
		pressedDown(),
		pressedLeft(),
		pressedRight(),

		pressedUpLeft(),
		pressedUpRight(),
		pressedDownLeft(),
		pressedDownRight(),

		getProcessedGamepad()->pressedB1(),
		getProcessedGamepad()->pressedB2(),
		getProcessedGamepad()->pressedB3(),
		getProcessedGamepad()->pressedB4(),
		getProcessedGamepad()->pressedL1(),
		getProcessedGamepad()->pressedR1(),
		getProcessedGamepad()->pressedL2(),
		getProcessedGamepad()->pressedR2(),
		getProcessedGamepad()->pressedS1(),
		getProcessedGamepad()->pressedS2(),
		getProcessedGamepad()->pressedL3(),
		getProcessedGamepad()->pressedR3(),
		getProcessedGamepad()->pressedA1(),
		getProcessedGamepad()->pressedA2(),
	};

	uint8_t mode = ((displayModeLookup.count(inputMode) > 0) ? displayModeLookup.at(inputMode) : 0);

	// Check if any new keys have been pressed
	if (lastInput != currentInput) {
		// Iterate through array
		for (uint8_t x=0; x<INPUT_HISTORY_MAX_INPUTS; x++) {
			// Add any pressed keys to deque
			std::string inputChar(displayNames[mode][x]);
			if (currentInput[x] && (inputChar != "")) pressed.push_back(inputChar);
		}
		// Update the last keypress array
		lastInput = currentInput;
	}

	if (pressed.size() > 0) {
		std::string newInput;
		for(const auto &s : pressed) {
				if(!newInput.empty())
						newInput += "+";
				newInput += s;
		}

		inputHistory.push_back(newInput);
	}

	if (inputHistory.size() > (inputHistoryLength / 2) + 1) {
		inputHistory.pop_front();
	}

	std::string ret;

	for (auto it = inputHistory.crbegin(); it != inputHistory.crend(); ++it) {
		std::string newRet = ret;
		if (!newRet.empty())
			newRet = " " + newRet;

		newRet = *it + newRet;
		ret = newRet;

		if (ret.size() >= inputHistoryLength) {
			break;
		}
	}

	if(ret.size() >= inputHistoryLength) {
		historyString = ret.substr(ret.size() - inputHistoryLength);
	} else {
		historyString = ret;
	}

    footer = historyString;
}

bool ButtonLayoutScreen::compareCustomLayouts()
{
    ButtonLayoutParamsLeft leftOptions = Storage::getInstance().getDisplayOptions().buttonLayoutCustomOptions.paramsLeft;
    ButtonLayoutParamsRight rightOptions = Storage::getInstance().getDisplayOptions().buttonLayoutCustomOptions.paramsRight;

    bool leftChanged = ((leftOptions.layout != prevLeftOptions.layout) || (leftOptions.common.startX != prevLeftOptions.common.startX) || (leftOptions.common.startY != prevLeftOptions.common.startY) || (leftOptions.common.buttonPadding != prevLeftOptions.common.buttonPadding) || (leftOptions.common.buttonRadius != prevLeftOptions.common.buttonRadius));
    bool rightChanged = ((rightOptions.layout != prevRightOptions.layout) || (rightOptions.common.startX != prevRightOptions.common.startX) || (rightOptions.common.startY != prevRightOptions.common.startY) || (rightOptions.common.buttonPadding != prevRightOptions.common.buttonPadding) || (rightOptions.common.buttonRadius != prevRightOptions.common.buttonRadius));
    
    return (leftChanged || rightChanged);
}

bool ButtonLayoutScreen::pressedUp()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == GAMEPAD_MASK_UP);
        case DPAD_MODE_LEFT_ANALOG:  return getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MIN;
        case DPAD_MODE_RIGHT_ANALOG: return getProcessedGamepad()->state.ry == GAMEPAD_JOYSTICK_MIN;
    }

    return false;
}

bool ButtonLayoutScreen::pressedDown()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == GAMEPAD_MASK_DOWN);
        case DPAD_MODE_LEFT_ANALOG:  return getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MAX;
        case DPAD_MODE_RIGHT_ANALOG: return getProcessedGamepad()->state.ry == GAMEPAD_JOYSTICK_MAX;
    }

    return false;
}

bool ButtonLayoutScreen::pressedLeft()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == GAMEPAD_MASK_LEFT);
        case DPAD_MODE_LEFT_ANALOG:  return getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MIN;
        case DPAD_MODE_RIGHT_ANALOG: return getProcessedGamepad()->state.rx == GAMEPAD_JOYSTICK_MIN;
    }

    return false;
}

bool ButtonLayoutScreen::pressedRight()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == GAMEPAD_MASK_RIGHT);
        case DPAD_MODE_LEFT_ANALOG:  return getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MAX;
        case DPAD_MODE_RIGHT_ANALOG: return getProcessedGamepad()->state.rx == GAMEPAD_JOYSTICK_MAX;
    }

    return false;
}

bool ButtonLayoutScreen::pressedUpLeft()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == (GAMEPAD_MASK_UP | GAMEPAD_MASK_LEFT));
        case DPAD_MODE_LEFT_ANALOG:  return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MIN) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MIN);
        case DPAD_MODE_RIGHT_ANALOG: return (getProcessedGamepad()->state.rx == GAMEPAD_JOYSTICK_MIN) && (getProcessedGamepad()->state.ry == GAMEPAD_JOYSTICK_MIN);
    }

    return false;
}

bool ButtonLayoutScreen::pressedUpRight()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == (GAMEPAD_MASK_UP | GAMEPAD_MASK_RIGHT));
        case DPAD_MODE_LEFT_ANALOG:  return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MAX) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MIN);
        case DPAD_MODE_RIGHT_ANALOG: return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MAX) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MIN);
    }

    return false;
}

bool ButtonLayoutScreen::pressedDownLeft()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == (GAMEPAD_MASK_DOWN | GAMEPAD_MASK_LEFT));
        case DPAD_MODE_LEFT_ANALOG:  return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MIN) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MAX);
        case DPAD_MODE_RIGHT_ANALOG: return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MIN) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MAX);
    }

    return false;
}

bool ButtonLayoutScreen::pressedDownRight()
{
    switch (getGamepad()->getActiveDpadMode())
    {
        case DPAD_MODE_DIGITAL:      return ((getProcessedGamepad()->state.dpad & GAMEPAD_MASK_DPAD) == (GAMEPAD_MASK_DOWN | GAMEPAD_MASK_RIGHT));
        case DPAD_MODE_LEFT_ANALOG:  return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MAX) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MAX);
        case DPAD_MODE_RIGHT_ANALOG: return (getProcessedGamepad()->state.lx == GAMEPAD_JOYSTICK_MAX) && (getProcessedGamepad()->state.ly == GAMEPAD_JOYSTICK_MAX);
    }

    return false;
}

void ButtonLayoutScreen::handleProfileChange(GPEvent* e) {
    GPProfileChangeEvent* event = (GPProfileChangeEvent*)e;

    profileNumber = event->currentValue;
    prevProfileNumber = event->previousValue;
}

void ButtonLayoutScreen::handleUSB(GPEvent* e) {
    GPUSBHostEvent* event = (GPUSBHostEvent*)e;
    bannerDelayStart = getMillis();
    prevProfileNumber = profileNumber;

    if (e->eventType() == GP_EVENT_USBHOST_MOUNT) {
        bannerMessage = "    USB Connected";
    } else if (e->eventType() == GP_EVENT_USBHOST_UNMOUNT) {
        bannerMessage = "  USB Disconnnected";
    }
    bannerDisplay = true;
}

void ButtonLayoutScreen::trim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            std::not1(std::ptr_fun<int, int>(std::isspace))));
}