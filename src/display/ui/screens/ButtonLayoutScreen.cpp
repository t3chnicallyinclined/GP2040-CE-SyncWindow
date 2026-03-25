#include "ButtonLayoutScreen.h"
#include "buttonlayouts.h"
#include "drivermanager.h"
#include "drivers/ps4/PS4Driver.h"
#include "drivers/xbone/XBOneDriver.h"
#include "drivers/xinput/XInputDriver.h"
#include "drivers/dreamcast/DreamcastDriver.h"

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

	// load layout (drawElement pushes element to the display list)
    // Skip widget loading in Dreamcast debug mode — we use the full display for debug text
    if (inputMode != INPUT_MODE_DREAMCAST) {
        uint16_t elementCtr = 0;
        LayoutManager::LayoutList currLayoutLeft = LayoutManager::getInstance().getLayoutA();
        LayoutManager::LayoutList currLayoutRight = LayoutManager::getInstance().getLayoutB();
        for (elementCtr = 0; elementCtr < currLayoutLeft.size(); elementCtr++) {
            pushElement(currLayoutLeft[elementCtr]);
        }
        for (elementCtr = 0; elementCtr < currLayoutRight.size(); elementCtr++) {
            pushElement(currLayoutRight[elementCtr]);
        }
    }

	// start with profile mode displayed
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

    // main logic loop
    if (prevProfileNumber != profileNumber) {
        bannerDelayStart = getMillis();
        prevProfileNumber = profileNumber;
        bannerDisplay = true;
    }

    // main logic loop
	generateHeader();
    if (isInputHistoryEnabled)
		processInputHistory();

    // check for exit/screen change
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
                    statusBar += "DC Rx:" + std::to_string(dc->debugRxCount);
                } else {
                    statusBar += "DC-NOINIT";
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
    // Dreamcast debug: take over entire display
    DreamcastDriver* dcDbg = DriverManager::getInstance().getDCDriver();
    if (inputMode == INPUT_MODE_DREAMCAST && dcDbg) {
        auto* dc = dcDbg;

        // Line 0: Rx/Tx counts and XOR failures
        std::string line0 = "Rx:" + std::to_string(dc->debugRxCount);
        line0 += " Tx:" + std::to_string(dc->debugTxCount);
        line0 += " XF:" + std::to_string(dc->debugXorFail);

        // Line 1: Per-command breakdown (what is DC sending us?)
        std::string line1 = "c1:" + std::to_string(dc->debugCmd1Count);
        line1 += " c2:" + std::to_string(dc->debugCmd2Count);
        line1 += " c9:" + std::to_string(dc->debugCmd9Count);

        // Line 2: VMU diagnostics — V:1/0=enabled/disabled, Vr=rx, Vt=tx, Vc=last cmd, Ve=last error
        char vmuBuf[64];
        snprintf(vmuBuf, sizeof(vmuBuf), "V:%d Vr:%lu Vt:%lu Vc:%d",
                 dc->disableVMU ? 0 : 1,
                 (unsigned long)dc->vmu.debugVmuRxCount,
                 (unsigned long)dc->vmu.debugVmuTxCount,
                 (int)dc->vmu.debugVmuLastCmd);
        std::string line2 = vmuBuf;

        // Lines 3-5: Find the most recent BLOCK_READ (0x0B) log entry
        // and show its raw bytes so we can see exact byte alignment
        std::string line3 = "", line4 = "", line5 = "";
        {
            char logBuf[64];
            uint16_t total = dc->vmu.cmdLogCount;
            uint16_t wIdx = dc->vmu.cmdLogWriteIdx;
            int avail = (total < VMU_LOG_MAX_ENTRIES) ? total : VMU_LOG_MAX_ENTRIES;

            // Find most recent FAILED BLOCK_READ (response=FILE_ERROR) and most recent OK one
            int failIdx = -1, okIdx = -1;
            for (int i = avail - 1; i >= 0; i--) {
                int idx = (wIdx - avail + i + VMU_LOG_MAX_ENTRIES) % VMU_LOG_MAX_ENTRIES;
                if (dc->vmu.cmdLog[idx].cmd == 0x0B) {
                    if (failIdx < 0 && dc->vmu.cmdLog[idx].response == (uint8_t)MAPLE_CMD_RESPOND_FILE_ERROR)
                        failIdx = idx;
                    if (okIdx < 0 && dc->vmu.cmdLog[idx].response == (uint8_t)MAPLE_CMD_RESPOND_DATA_XFER)
                        okIdx = idx;
                }
                if (failIdx >= 0 && okIdx >= 0) break;
            }
            int foundIdx = (failIdx >= 0) ? failIdx : okIdx;

            // Line 3: FAIL — hdr[4] + payload[8] = 12 bytes
            // Format: "F H:XXXXXXXX XXXXXXXX" = header then locWord (skip funcCode)
            // rawBytes[0..3]=header, [4..7]=funcCode, [8..11]=locWord
            if (failIdx >= 0) {
                const VmuLogEntry& ef = dc->vmu.cmdLog[failIdx];
                snprintf(logBuf, sizeof(logBuf), "F%02X%02X%02X%02X %02X%02X%02X%02X",
                         ef.rawBytes[0], ef.rawBytes[1], ef.rawBytes[2], ef.rawBytes[3],
                         ef.rawBytes[8], ef.rawBytes[9], ef.rawBytes[10], ef.rawBytes[11]);
                line3 = logBuf;
            } else {
                line3 = "F: none";
            }
            // Line 4: OK — same format
            if (okIdx >= 0) {
                const VmuLogEntry& eo = dc->vmu.cmdLog[okIdx];
                snprintf(logBuf, sizeof(logBuf), "O%02X%02X%02X%02X %02X%02X%02X%02X",
                         eo.rawBytes[0], eo.rawBytes[1], eo.rawBytes[2], eo.rawBytes[3],
                         eo.rawBytes[8], eo.rawBytes[9], eo.rawBytes[10], eo.rawBytes[11]);
                line4 = logBuf;
            } else {
                line4 = "O: none";
            }
            // Line 5: counts + flash write status
            snprintf(logBuf, sizeof(logBuf), "ok%lu er%lu fw%lu fb%u",
                     (unsigned long)dc->vmu.debugVmuReadOkCount,
                     (unsigned long)dc->vmu.debugVmuReadErrCount,
                     (unsigned long)dc->vmu.debugVmuFlashCount,
                     (unsigned)dc->vmu.debugVmuLastFlashBlock);
            line5 = logBuf;
        }

        getRenderer()->drawText(0, 0, line0);
        getRenderer()->drawText(0, 1, line1);
        getRenderer()->drawText(0, 2, line2);
        getRenderer()->drawText(0, 3, line3);
        getRenderer()->drawText(0, 4, line4);
        getRenderer()->drawText(0, 5, line5);
        return;
    }

    if (bannerDisplay) {
        getRenderer()->drawRectangle(0, 0, 128, 7, true, true);
    	getRenderer()->drawText(0, 0, statusBar, true);
    } else {
		getRenderer()->drawText(0, 0, statusBar);
	}
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