#include "RepRapFirmware.h"

// RepRap member functions.

// Do nothing more in the constructor; put what you want in RepRap:Init()

RepRap::RepRap() : lastToolWarningTime(0.0), ticksInSpinState(0), spinningModule(noModule),	debug(0),
	stopped(false), active(false), resetting(false)
{
	OutputBuffer::Init();
	platform = new Platform();
	network = new Network(platform);
	webserver = new Webserver(platform, network);
	gCodes = new GCodes(platform, webserver);
	move = new Move(platform, gCodes);
	heat = new Heat(platform);
	roland = new Roland(platform);
	printMonitor = new PrintMonitor(platform, gCodes);

	toolList = nullptr;
}

void RepRap::Init()
{
	debug = 0;

	// chrishamm thinks it's a bad idea to count the bed as an active heater...
	activeExtruders = activeHeaters = 0;

	SetPassword(DEFAULT_PASSWORD);
	SetName(DEFAULT_NAME);

	beepFrequency = beepDuration = 0;
	message[0] = 0;

	processingConfig = true;

	// All of the following init functions must execute reasonably quickly before the watchdog times us out
	platform->Init();
	gCodes->Init();
	network->Init();
	webserver->Init();
	move->Init();
	heat->Init();
	roland->Init();
	printMonitor->Init();
	currentTool = nullptr;

	active = true;		// must do this before we start the network, else the watchdog may time out

	platform->MessageF(HOST_MESSAGE, "%s Version %s dated %s\n", NAME, VERSION, DATE);

	const char *configFile = platform->GetConfigFile();
	platform->Message(HOST_MESSAGE, "\nExecuting ");
	if (platform->GetMassStorage()->FileExists(platform->GetSysDir(), configFile))
	{
		platform->MessageF(HOST_MESSAGE, "%s...", platform->GetConfigFile());
	}
	else
	{
		platform->MessageF(HOST_MESSAGE, "%s (no configuration file found)...", platform->GetDefaultFile());
		configFile = platform->GetDefaultFile();
	}

	while (!gCodes->DoFileMacro(nullptr, configFile))
	{
		// GCodes::Spin will read the macro and ensure DoFileMacro returns true when it's done
		Spin();
	}
	processingConfig = false;
	platform->Message(HOST_MESSAGE, " Done!\n");

	if (network->IsEnabled())
	{
		// Need to do this here, as the configuration GCodes may set IP address etc.
		platform->Message(HOST_MESSAGE, "Starting network...\n");
		network->Enable();
	}
	else
	{
		platform->Message(HOST_MESSAGE, "Network disabled.\n");
	}

	platform->MessageF(HOST_MESSAGE, "%s is up and running.\n\n", NAME);
	fastLoop = FLT_MAX;
	slowLoop = 0.0;
	lastTime = platform->Time();
}

void RepRap::Exit()
{
	active = false;
	heat->Exit();
	move->Exit();
	gCodes->Exit();
	webserver->Exit();
	platform->Message(GENERIC_MESSAGE, "RepRap class exited.\n");
	platform->Exit();
}

void RepRap::Spin()
{
	if (!active)
		return;

	spinningModule = modulePlatform;
	ticksInSpinState = 0;
	platform->Spin();

	spinningModule = moduleNetwork;
	ticksInSpinState = 0;
	network->Spin();

	spinningModule = moduleWebserver;
	ticksInSpinState = 0;
	webserver->Spin();

	spinningModule = moduleGcodes;
	ticksInSpinState = 0;
	gCodes->Spin();

	spinningModule = moduleMove;
	ticksInSpinState = 0;
	move->Spin();

	spinningModule = moduleHeat;
	ticksInSpinState = 0;
	heat->Spin();

	spinningModule = moduleRoland;
	ticksInSpinState = 0;
	roland->Spin();

	spinningModule = modulePrintMonitor;
	ticksInSpinState = 0;
	printMonitor->Spin();

	spinningModule = noModule;
	ticksInSpinState = 0;

	// Check if we need to display a cold extrusion warning

	for(Tool *t = toolList; t != nullptr; t = t->Next())
	{
		if (t->DisplayColdExtrudeWarning() && ToolWarningsAllowed())
		{
			platform->MessageF(GENERIC_MESSAGE, "Warning: Tool %d was not driven because its heater temperatures were not high enough\n", t->Number());
			break;
		}
	}

	// Keep track of the loop time

	float t = platform->Time();
	float dt = t - lastTime;
	if (dt < fastLoop)
	{
		fastLoop = dt;
	}
	if (dt > slowLoop)
	{
		slowLoop = dt;
	}
	lastTime = t;
}

void RepRap::Timing()
{
	platform->MessageF(GENERIC_MESSAGE, "Slowest main loop (seconds): %f; fastest: %f\n", slowLoop, fastLoop);
	fastLoop = FLT_MAX;
	slowLoop = 0.0;
}

void RepRap::Diagnostics()
{
	platform->Message(GENERIC_MESSAGE, "Diagnostics\n");
	OutputBuffer::Diagnostics();
	platform->Diagnostics();				// this includes a call to our Timing() function
	move->Diagnostics();
	heat->Diagnostics();
	gCodes->Diagnostics();
	network->Diagnostics();
	webserver->Diagnostics();
}

// Turn off the heaters, disable the motors, and
// deactivate the Heat and Move classes.  Leave everything else
// working.

void RepRap::EmergencyStop()
{
	stopped = true;
	platform->SetAtxPower(false);		// turn off the ATX power if we can

	Tool* tool = toolList;
	while(tool)
	{
		tool->Standby();
		tool = tool->Next();
	}

	heat->Exit();
	for(size_t heater = 0; heater < HEATERS; heater++)
	{
		platform->SetHeater(heater, 0.0);
	}

	// We do this twice, to avoid an interrupt switching
	// a drive back on.  move->Exit() should prevent
	// interrupts doing this.

	for(size_t i = 0; i < 2; i++)
	{
		move->Exit();
		for(size_t drive = 0; drive < DRIVES; drive++)
		{
			platform->SetMotorCurrent(drive, 0.0);
			platform->DisableDrive(drive);
		}
	}
}

void RepRap::SetDebug(Module m, bool enable)
{
	if (enable)
	{
		debug |= (1 << m);
	}
	else
	{
		debug &= ~(1 << m);
	}
	PrintDebug();
}

void RepRap::SetDebug(bool enable)
{
	debug = (enable) ? 0xFFFF : 0;
}

void RepRap::PrintDebug()
{
	if (debug != 0)
	{
		platform->Message(GENERIC_MESSAGE, "Debugging enabled for modules:");
		for(size_t i = 0; i < numModules; i++)
		{
			if ((debug & (1 << i)) != 0)
			{
				platform->MessageF(GENERIC_MESSAGE, " %s (%u)", moduleName[i], i);
			}
		}

		platform->Message(GENERIC_MESSAGE, "\nDebugging disabled for modules:");
		for(size_t i = 0; i < numModules; i++)
		{
			if ((debug & (1 << i)) == 0)
			{
				platform->MessageF(GENERIC_MESSAGE, " %s(%u)", moduleName[i], i);
			}
		}
		platform->Message(GENERIC_MESSAGE, "\n");
	}
	else
	{
		platform->Message(GENERIC_MESSAGE, "Debugging disabled\n");
	}
}

void RepRap::AddTool(Tool* tool)
{
	if (toolList == nullptr)
	{
		toolList = tool;
	}
	else
	{
		toolList->AddTool(tool);
	}
	tool->UpdateExtruderAndHeaterCount(activeExtruders, activeHeaters);
}

void RepRap::DeleteTool(Tool* tool)
{
	// Must have a valid tool...
	if (tool == nullptr)
	{
		return;
	}

	// Deselect it if necessary
	if (GetCurrentTool() == tool)
	{
		SelectTool(-1);
	}

	// Switch off any associated heater
	for(size_t i=0; i<tool->HeaterCount(); i++)
	{
		reprap.GetHeat()->SwitchOff(tool->Heater(i));
	}

	// Purge any references to this tool
	Tool *parent = nullptr;
	for(Tool *t = toolList; t != nullptr; t = t->Next())
	{
		if (t->Next() == tool)
		{
			parent = t;
			break;
		}
	}

	if (parent == nullptr)
	{
		toolList = tool->Next();
	}
	else
	{
		parent->next = tool->next;
	}

	// Delete it
	delete tool;

	// Update the number of active heaters and extruder drives
	activeExtruders = activeHeaters = 0;
	for(Tool *t = toolList; t != nullptr; t = t->Next())
	{
		t->UpdateExtruderAndHeaterCount(activeExtruders, activeHeaters);
	}
}

void RepRap::SelectTool(int toolNumber)
{
	Tool* tool = toolList;

	while (tool)
	{
		if (tool->Number() == toolNumber)
		{
			tool->Activate(currentTool);
			currentTool = tool;
			return;
		}
		tool = tool->Next();
	}

	// Selecting a non-existent tool is valid.  It sets them all to standby.

	if (currentTool != nullptr)
	{
		StandbyTool(currentTool->Number());
	}
	currentTool = nullptr;
}

void RepRap::PrintTool(int toolNumber, StringRef& reply)
{
	for(Tool *tool = toolList; tool != nullptr; tool = tool->next)
	{
		if (tool->Number() == toolNumber)
		{
			tool->Print(reply);
			return;
		}
	}

	reply.copy("Error: Attempt to print details of non-existent tool.\n");
}

void RepRap::StandbyTool(int toolNumber)
{
	Tool* tool = toolList;

	while (tool)
	{
		if (tool->Number() == toolNumber)
		{
			tool->Standby();
			if (currentTool == tool)
			{
				currentTool = nullptr;
			}
			return;
		}
		tool = tool->Next();
	}

	platform->MessageF(GENERIC_MESSAGE, "Error: Attempt to standby a non-existent tool: %d.\n", toolNumber);
}

Tool* RepRap::GetTool(int toolNumber) const
{
	Tool* tool = toolList;

	while (tool)
	{
		if(tool->Number() == toolNumber)
		{
			return tool;
		}

		tool = tool->Next();
	}
	return nullptr; // Not an error
}

Tool* RepRap::GetOnlyTool() const
{
	return (toolList != nullptr && toolList->Next() == nullptr) ? toolList : nullptr;
}

/*Tool* RepRap::GetToolByDrive(int driveNumber)
{
	Tool* tool = toolList;

	while (tool)
	{
		for(size_t drive = 0; drive < tool->DriveCount(); drive++)
		{
			if (tool->Drive(drive) + AXES == driveNumber)
			{
				return tool;
			}
		}

		tool = tool->Next();
	}
	return nullptr;
}*/

void RepRap::SetToolVariables(int toolNumber, const float* standbyTemperatures, const float* activeTemperatures)
{
	Tool* tool = toolList;

	while (tool)
	{
		if (tool->Number() == toolNumber)
		{
			tool->SetVariables(standbyTemperatures, activeTemperatures);
			return;
		}
		tool = tool->Next();
	}

	platform->MessageF(GENERIC_MESSAGE, "Error: Attempt to set variables for a non-existent tool: %d.\n", toolNumber);
}

// chrishamm 02-10-2015: I don't think it's a good idea to write tool warning message after every
// short move, so only print them in a reasonable interval.
bool RepRap::ToolWarningsAllowed()
{
	const float now = platform->Time();
	if (now - lastToolWarningTime > MINIMUM_TOOL_WARNING_INTERVAL)
	{
		lastToolWarningTime = platform->Time();
		return true;
	}
	return false;
}

bool RepRap::IsHeaterAssignedToTool(int8_t heater) const
{
	for(Tool *tool = toolList; tool != nullptr; tool = tool->Next())
	{
		for(size_t i = 0; i < tool->HeaterCount(); i++)
		{
			if (tool->Heater(i) == heater)
			{
				// It's already in use by some tool
				return true;
			}
		}
	}

	return false;
}

void RepRap::Tick()
{
	if (active && !resetting)
	{
		platform->Tick();
		++ticksInSpinState;
		if (ticksInSpinState >= 20000)	// if we stall for 20 seconds, save diagnostic data and reset
		{
			resetting = true;
			for(size_t i = 0; i < HEATERS; i++)
			{
				platform->SetHeater(i, 0.0);
			}
			for(size_t i = 0; i < DRIVES; i++)
			{
				platform->DisableDrive(i);
				// We can't set motor currents to 0 here because that requires interrupts to be working, and we are in an ISR
			}

			platform->SoftwareReset(SoftwareResetReason::stuckInSpin);
		}
	}
}
// Get the JSON status response for the web server (or later for the M105 command).
// Type 1 is the ordinary JSON status response.
// Type 2 is the same except that static parameters are also included.
// Type 3 is the same but instead of static parameters we report print estimation values.
OutputBuffer *RepRap::GetStatusResponse(uint8_t type, ResponseSource source)
{
	// Need something to write to...
	OutputBuffer *response;
	if (!OutputBuffer::Allocate(response))
	{
		// Should never happen
		return nullptr;
	}

	// Machine status
	char ch = GetStatusCharacter();
	response->printf("{\"status\":\"%c\",\"coords\":{", ch);

	/* Coordinates */
	{
		float liveCoordinates[DRIVES + 1];
		if (roland->Active())
		{
			roland->GetCurrentRolandPosition(liveCoordinates);
		}
		else
		{
			move->LiveCoordinates(liveCoordinates);
		}

		if (currentTool != nullptr)
		{
			const float *offset = currentTool->GetOffset();
			for (size_t i = 0; i < AXES; ++i)
			{
				liveCoordinates[i] += offset[i];
			}
		}

		// Homed axes
		response->catf("\"axesHomed\":[%d,%d,%d]",
				(gCodes->GetAxisIsHomed(0)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(1)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(2)) ? 1 : 0);

		// Actual and theoretical extruder positions since power up, last G92 or last M23
		response->catf(",\"extr\":");		// announce actual extruder positions
		ch = '[';
		for (size_t extruder = 0; extruder < GetExtrudersInUse(); extruder++)
		{
			response->catf("%c%.1f", ch, liveCoordinates[AXES + extruder]);
			ch = ',';
		}
		if (ch == '[')
		{
			response->cat("[");
		}

		// XYZ positions
		response->cat("],\"xyz\":");
		if (!gCodes->AllAxesAreHomed() && move->IsDeltaMode())
		{
			// If in Delta mode, skip these coordinates if some axes are not homed
			response->cat("[0.00,0.00,0.00");
		}
		else
		{
			// On Cartesian printers, the live coordinates are (usually) valid
			ch = '[';
			for (size_t axis = 0; axis < AXES; axis++)
			{
				response->catf("%c%.2f", ch, liveCoordinates[axis]);
				ch = ',';
			}
		}
	}

	// Current tool number
	int toolNumber = (currentTool == nullptr) ? -1 : currentTool->Number();
	response->catf("]},\"currentTool\":%d", toolNumber);

	/* Output - only reported once */
	{
		bool sendBeep = (beepDuration != 0 && beepFrequency != 0);
		bool sendMessage = (message[0] != 0);
		bool sourceRight = (gCodes->HaveAux() && source == ResponseSource::AUX) || (!gCodes->HaveAux() && source == ResponseSource::HTTP);
		if ((sendBeep || message[0] != 0) && sourceRight)
		{
			response->cat(",\"output\":{");

			// Report beep values
			if (sendBeep)
			{
				response->catf("\"beepDuration\":%d,\"beepFrequency\":%d", beepDuration, beepFrequency);
				if (sendMessage)
				{
					response->cat(",");
				}

				beepFrequency = beepDuration = 0;
			}

			// Report message
			if (sendMessage)
			{
				response->cat("\"message\":");
				response->EncodeString(message, ARRAY_SIZE(message), false);
				message[0] = 0;
			}
			response->cat("}");
		}
	}

	/* Parameters */
	{
		// ATX power
		response->catf(",\"params\":{\"atxPower\":%d", platform->AtxPower() ? 1 : 0);

		// Cooling fan value
		response->cat(",\"fanPercent\":[");
		for(size_t i = 0; i < NUM_FANS; i++)
		{
			if (i == NUM_FANS - 1)
			{
				response->catf("%.2f", platform->GetFanValue(i) * 100.0);
			}
			else
			{
				response->catf("%.2f,", platform->GetFanValue(i) * 100.0);
			}
		}

		// Speed and Extrusion factors
		response->catf("],\"speedFactor\":%.2f,\"extrFactors\":", move->GetSpeedFactor() * 100.0);
		ch = '[';
		for (size_t extruder = 0; extruder < GetExtrudersInUse(); extruder++)
		{
			response->catf("%c%.2f", ch, move->GetExtrusionFactor(extruder) * 100.0);
			ch = ',';
		}
		response->cat((ch == '[') ? "[]}" : "]}");
	}

	// G-code reply sequence for webserver (seqence number for AUX is handled later)
	if (source == ResponseSource::HTTP)
	{
		response->catf(",\"seq\":%d", webserver->GetReplySeq());

		// There currently appears to be no need for this one, so skip it
		//response->catf(",\"buff\":%u", webserver->GetGCodeBufferSpace(WebSource::HTTP));
	}

	/* Sensors */
	{
		response->cat(",\"sensors\":{");

		// Probe
		int v0 = platform->ZProbe();
		int v1, v2;
		switch (platform->GetZProbeSecondaryValues(v1, v2))
		{
			case 1:
				response->catf("\"probeValue\":%d,\"probeSecondary\":[%d]", v0, v1);
				break;
			case 2:
				response->catf("\"probeValue\":%d,\"probeSecondary\":[%d,%d]", v0, v1, v2);
				break;
			default:
				response->catf("\"probeValue\":%d", v0);
				break;
		}

		// Fan RPM
		response->catf(",\"fanRPM\":%d}", static_cast<unsigned int>(platform->GetFanRPM()));
	}

	/* Temperatures */
	{
		response->cat(",\"temps\":{");

		/* Bed */
		const int8_t bedHeater = heat->GetBedHeater();
		if (bedHeater != -1)
		{
			response->catf("\"bed\":{\"current\":%.1f,\"active\":%.1f,\"state\":%d},",
					heat->GetTemperature(bedHeater), heat->GetActiveTemperature(bedHeater),
					heat->GetStatus(bedHeater));
		}

		/* Chamber */
		const int8_t chamberHeater = heat->GetChamberHeater();
		if (chamberHeater != -1)
		{
			response->catf("\"chamber\":{\"current\":%.1f,", heat->GetTemperature(chamberHeater));
			response->catf("\"active\":%.1f,", heat->GetActiveTemperature(chamberHeater));
			response->catf("\"state\":%d},", static_cast<int>(heat->GetStatus(chamberHeater)));
		}

		/* Heads */
		{
			response->cat("\"heads\":{\"current\":");

			// Current temperatures
			ch = '[';
			for (size_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response->catf("%c%.1f", ch, heat->GetTemperature(heater));
				ch = ',';
			}
			response->cat((ch == '[') ? "[]" : "]");

			// Active temperatures
			response->catf(",\"active\":");
			ch = '[';
			for (size_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response->catf("%c%.1f", ch, heat->GetActiveTemperature(heater));
				ch = ',';
			}
			response->cat((ch == '[') ? "[]" : "]");

			// Standby temperatures
			response->catf(",\"standby\":");
			ch = '[';
			for (size_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response->catf("%c%.1f", ch, heat->GetStandbyTemperature(heater));
				ch = ',';
			}
			response->cat((ch == '[') ? "[]" : "]");

			// Heater statuses (0=off, 1=standby, 2=active, 3=fault)
			response->cat(",\"state\":");
			ch = '[';
			for (size_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response->catf("%c%d", ch, static_cast<int>(heat->GetStatus(heater)));
				ch = ',';
			}
			response->cat((ch == '[') ? "[]" : "]");
		}
		response->cat("}}");
	}

	// Time since last reset
	response->catf(",\"time\":%.1f", platform->Time());

	/* Extended Status Response */
	if (type == 2)
	{
		// Cold Extrude/Retract
		response->catf(",\"coldExtrudeTemp\":%1.f", heat->ColdExtrude() ? 0 : HOT_ENOUGH_TO_EXTRUDE);
		response->catf(",\"coldRetractTemp\":%1.f", heat->ColdExtrude() ? 0 : HOT_ENOUGH_TO_RETRACT);

		// Endstops
		uint16_t endstops = 0;
		for(size_t drive = 0; drive < DRIVES; drive++)
		{
			EndStopHit stopped = platform->Stopped(drive);
			if (stopped == EndStopHit::highHit || stopped == EndStopHit::lowHit)
			{
				endstops |= (1 << drive);
			}
		}
		response->catf(",\"endstops\":%d", endstops);

		// Delta configuration
		response->catf(",\"geometry\":\"%s\"", move->GetGeometryString());

		// Machine name
		response->cat(",\"name\":");
		response->EncodeString(myName, ARRAY_SIZE(myName), false);

		/* Probe */
		{
			const ZProbeParameters probeParams = platform->GetZProbeParameters();

			// Trigger threshold
			response->catf(",\"probe\":{\"threshold\":%d", probeParams.adcValue);

			// Trigger height
			response->catf(",\"height\":%.2f", probeParams.height);

			// Type
			response->catf(",\"type\":%d}", platform->GetZProbeType());
		}

		/* Tool Mapping */
		{
			response->cat(",\"tools\":[");
			for(Tool *tool = toolList; tool != nullptr; tool = tool->Next())
			{
				// Heaters
				response->catf("{\"number\":%d,\"heaters\":[", tool->Number());
				for(size_t heater = 0; heater < tool->HeaterCount(); heater++)
				{
					response->catf("%d", tool->Heater(heater));
					if (heater + 1 < tool->HeaterCount())
					{
						response->cat(",");
					}
				}

				// Extruder drives
				response->cat("],\"drives\":[");
				for(size_t drive = 0; drive < tool->DriveCount(); drive++)
				{
					response->catf("%d", tool->Drive(drive));
					if (drive + 1 < tool->DriveCount())
					{
						response->cat(",");
					}
				}

				// Do we have any more tools?
				if (tool->Next() != nullptr)
				{
					response->cat("]},");
				}
				else
				{
					response->cat("]}");
				}
			}
			response->cat("]");
		}
	}
	else if (type == 3)
	{
		// Current Layer
		response->catf(",\"currentLayer\":%d", printMonitor->GetCurrentLayer());

		// Current Layer Time
		response->catf(",\"currentLayerTime\":%.1f", printMonitor->GetCurrentLayerTime());

		// Raw Extruder Positions
		float rawExtruderTotals[DRIVES - AXES];
		move->RawExtruderTotals(rawExtruderTotals);
		response->cat(",\"extrRaw\":");
		ch = '[';
		for (size_t extruder = 0; extruder < GetExtrudersInUse(); extruder++)		// loop through extruders
		{
			response->catf("%c%.1f", ch, rawExtruderTotals[extruder]);
			ch = ',';
		}
		if (ch == '[')
		{
			response->cat("]");
		}

		// Fraction of file printed
		response->catf("],\"fractionPrinted\":%.1f", (printMonitor->IsPrinting()) ? (gCodes->FractionOfFilePrinted() * 100.0) : 0.0);

		// First Layer Duration
		response->catf(",\"firstLayerDuration\":%.1f", printMonitor->GetFirstLayerDuration());

		// First Layer Height
		// NB: This shouldn't be needed any more, but leave it here for the case that the file-based first-layer detection fails
		response->catf(",\"firstLayerHeight\":%.2f", printMonitor->GetFirstLayerHeight());

		// Print Duration
		response->catf(",\"printDuration\":%.1f", printMonitor->GetPrintDuration());

		// Warm-Up Time
		response->catf(",\"warmUpDuration\":%.1f", printMonitor->GetWarmUpDuration());

		/* Print Time Estimations */
		{
			// Based on file progress
			response->catf(",\"timesLeft\":{\"file\":%.1f", printMonitor->EstimateTimeLeft(fileBased));

			// Based on filament usage
			response->catf(",\"filament\":%.1f", printMonitor->EstimateTimeLeft(filamentBased));

			// Based on layers
			response->catf(",\"layer\":%.1f}", printMonitor->EstimateTimeLeft(layerBased));
		}
	}

	if (source == ResponseSource::AUX)
	{
		OutputBuffer *response = gCodes->GetAuxGCodeReply();
		if (response != nullptr)
		{
			// Send the response to the last command. Do this last
			response->catf(",\"seq\":%u,\"resp\":", gCodes->GetAuxSeq());			// send the response sequence number

			// Send the JSON response
			response->EncodeReply(response, true);									// also releases the OutputBuffer chain
		}
	}
	response->cat("}");

	return response;
}

OutputBuffer *RepRap::GetConfigResponse()
{
	// We need some resources to return a valid config response...
	OutputBuffer *response;
	if (!OutputBuffer::Allocate(response))
	{
		return nullptr;
	}

	// Axis minima
	response->copy("{\"axisMins\":");
	char ch = '[';
	for (size_t axis = 0; axis < AXES; axis++)
	{
		response->catf("%c%.2f", ch, platform->AxisMinimum(axis));
		ch = ',';
	}

	// Axis maxima
	response->cat("],\"axisMaxes\":");
	ch = '[';
	for (size_t axis = 0; axis < AXES; axis++)
	{
		response->catf("%c%.2f", ch, platform->AxisMaximum(axis));
		ch = ',';
	}

	// Accelerations
	response->cat("],\"accelerations\":");
	ch = '[';
	for (size_t drive = 0; drive < DRIVES; drive++)
	{
		response->catf("%c%.2f", ch, platform->Acceleration(drive));
		ch = ',';
	}

	// Motor currents
	response->cat("],\"currents\":");
	ch = '[';
	for (size_t drive = 0; drive < DRIVES; drive++)
	{
		response->catf("%c%.2f", ch, platform->MotorCurrent(drive));
		ch = ',';
	}

	// Firmware details
	response->catf("],\"firmwareElectronics\":\"%s\"", ELECTRONICS);
	response->catf(",\"firmwareName\":\"%s\"", NAME);
	response->catf(",\"firmwareVersion\":\"%s\"", VERSION);
	response->catf(",\"firmwareDate\":\"%s\"", DATE);

	// Motor idle parameters
	response->catf(",\"idleCurrentFactor\":%.1f", platform->GetIdleCurrentFactor() * 100.0);
	response->catf(",\"idleTimeout\":%.1f", move->IdleTimeout());

	// Minimum feedrates
	response->cat(",\"minFeedrates\":");
	ch = '[';
	for (size_t drive = 0; drive < DRIVES; drive++)
	{
		response->catf("%c%.2f", ch, platform->ConfiguredInstantDv(drive));
		ch = ',';
	}

	// Maximum feedrates
	response->cat("],\"maxFeedrates\":");
	ch = '[';
	for (size_t drive = 0; drive < DRIVES; drive++)
	{
		response->catf("%c%.2f", ch, platform->MaxFeedrate(drive));
		ch = ',';
	}

	// Configuration File (whitespaces are skipped, otherwise we easily risk overflowing the response buffer)
	response->cat("],\"configFile\":\"");
	FileStore *configFile = platform->GetFileStore(platform->GetSysDir(), platform->GetConfigFile(), false);
	if (configFile == nullptr)
	{
		response->cat("not found");
	}
	else
	{
		char c, esc;
		bool readingWhitespace = false;
		size_t bytesWritten = 0, bytesLeft = OutputBuffer::GetBytesLeft(response);
		while (configFile->Read(c) && bytesWritten + 4 < bytesLeft)		// need 4 bytes to finish this response
		{
			if (!readingWhitespace || (c != ' ' && c != '\t'))
			{
				switch (c)
				{
					case '\r':
						esc = 'r';
						break;
					case '\n':
						esc = 'n';
						break;
					case '\t':
						esc = 't';
						break;
					case '"':
						esc = '"';
						break;
					case '\\':
						esc = '\\';
						break;
					default:
						esc = 0;
						break;
				}

				if (esc)
				{
					response->catf("\\%c", esc);
					bytesWritten += 2;
				}
				else
				{
					response->cat(c);
					bytesWritten++;
				}
			}
			readingWhitespace = (c == ' ' || c == '\t');
		}
		configFile->Close();
	}
	response->cat("\"}");

	return response;
}

// Get the legacy JSON status response for the web server or M105 command.
// Type 0 is the old-style webserver status response (zpl fork doesn't support it any more).
// Type 1 is the new-style webserver status response.
// Type 2 is the M105 S2 response, which is like the new-style status response but some fields are omitted.
// Type 3 is the M105 S3 response, which is like the M105 S2 response except that static values are also included.
// 'seq' is the response sequence number, if it is not -1 and we have a different sequence number then we send the gcode response
OutputBuffer *RepRap::GetLegacyStatusResponse(uint8_t type, int seq)
{
	// Need something to write to...
	OutputBuffer *response;
	if (!OutputBuffer::Allocate(response))
	{
		// Should never happen
		return nullptr;
	}

	const GCodes *gc = reprap.GetGCodes();
	if (type != 0)
	{
		// Send the status. Note that 'S' has always meant that the machine is halted in this version of the status response-> so we use A for pAused.
		char ch = GetStatusCharacter();
		if (ch == 'S')			// if paused then send 'A'
		{
			ch = 'A';
		}
		else if (ch == 'H')		// if halted then send 'S'
		{
			ch = 'S';
		}
		response->printf("{\"status\":\"%c\",\"heaters\":", ch);

		// Send the heater actual temperatures
		const int8_t bedHeater = heat->GetBedHeater();
		if (bedHeater != -1)
		{
			ch = ',';
			response->catf("[%.1f", heat->GetTemperature(bedHeater));
		}
		else
		{
			ch = '[';
		}
		for (size_t heater = E0_HEATER; heater < reprap.GetHeatersInUse(); heater++)
		{
			response->catf("%c%.1f", ch, heat->GetTemperature(heater));
			ch = ',';
		}
		response->cat((ch == '[') ? "[]" : "]");

		// Send the heater active temperatures
		response->catf(",\"active\":");
		if (heat->GetBedHeater() != -1)
		{
			ch = ',';
			response->catf("[%.1f", heat->GetActiveTemperature(heat->GetBedHeater()));
		}
		else
		{
			ch = '[';
		}
		for (size_t heater = E0_HEATER; heater < reprap.GetHeatersInUse(); heater++)
		{
			response->catf("%c%.1f", ch, heat->GetActiveTemperature(heater));
			ch = ',';
		}
		response->cat((ch == '[') ? "[]" : "]");

		// Send the heater standby temperatures
		response->catf(",\"standby\":");
		if (bedHeater != -1)
		{
			ch = ',';
			response->catf("[%.1f", heat->GetStandbyTemperature(bedHeater));
		}
		else
		{
			ch = '[';
		}
		for (size_t heater = E0_HEATER; heater < reprap.GetHeatersInUse(); heater++)
		{
			response->catf("%c%.1f", ch, heat->GetStandbyTemperature(heater));
			ch = ',';
		}
		response->cat((ch == '[') ? "[]" : "]");

		// Send the heater statuses (0=off, 1=standby, 2=active)
		response->cat(",\"hstat\":");
		if (bedHeater != -1)
		{
			ch = ',';
			response->catf("[%d", static_cast<int>(heat->GetStatus(bedHeater)));
		}
		else
		{
			ch = '[';
		}
		for (size_t heater = E0_HEATER; heater < reprap.GetHeatersInUse(); heater++)
		{
			response->catf("%c%d", ch, static_cast<int>(heat->GetStatus(heater)));
			ch = ',';
		}
		response->cat((ch == '[') ? "[]" : "]");

		// Send XYZ positions
		float liveCoordinates[DRIVES + 1];
		reprap.GetMove()->LiveCoordinates(liveCoordinates);
		const Tool* currentTool = reprap.GetCurrentTool();
		if (currentTool != nullptr)
		{
			const float *offset = currentTool->GetOffset();
			for (size_t i = 0; i < AXES; ++i)
			{
				liveCoordinates[i] += offset[i];
			}
		}
		response->catf(",\"pos\":");		// announce the XYZ position
		ch = '[';
		for (size_t drive = 0; drive < AXES; drive++)
		{
			response->catf("%c%.2f", ch, liveCoordinates[drive]);
			ch = ',';
		}

		// Send extruder total extrusion since power up, last G92 or last M23
		response->cat("],\"extr\":");		// announce the extruder positions
		ch = '[';
		for (size_t drive = 0; drive < reprap.GetExtrudersInUse(); drive++)		// loop through extruders
		{
			response->catf("%c%.1f", ch, liveCoordinates[drive + AXES]);
			ch = ',';
		}
		response->cat((ch == ']') ? "[]" : "]");

		// Send the speed and extruder override factors
		response->catf(",\"sfactor\":%.2f,\"efactor\":", move->GetSpeedFactor() * 100.0);
		ch = '[';
		for (size_t i = 0; i < reprap.GetExtrudersInUse(); ++i)
		{
			response->catf("%c%.2f", ch, move->GetExtrusionFactor(i) * 100.0);
			ch = ',';
		}
		response->cat((ch == '[') ? "[]" : "]");

		// Send the current tool number
		int toolNumber = (currentTool == nullptr) ? 0 : currentTool->Number();
		response->catf(",\"tool\":%d", toolNumber);
	}
	else
	{
		// The old (deprecated) poll response->lists the status, then all the heater temperatures, then the XYZ positions, then all the extruder positions.
		// These are all returned in a single vector called "poll".
		// This is a poor choice of format because we can't easily tell which is which unless we already know the number of heaters and extruders.
		// RRP reversed the order at version 0.65 to send the positions before the heaters, but we haven't yet done that.
		char c = (printMonitor->IsPrinting()) ? 'P' : 'I';
		response->printf("{\"poll\":[\"%c\",", c); // Printing
		for (size_t heater = 0; heater < HEATERS; heater++)
		{
			response->catf("\"%.1f\",", reprap.GetHeat()->GetTemperature(heater));
		}
		// Send XYZ and extruder positions
		float liveCoordinates[DRIVES + 1];
		reprap.GetMove()->LiveCoordinates(liveCoordinates);
		for (size_t drive = 0; drive < DRIVES; drive++)	// loop through extruders
		{
			char ch = (drive == DRIVES - 1) ? ']' : ',';	// append ] to the last one but , to the others
			response->catf("\"%.2f\"%c", liveCoordinates[drive], ch);
		}
	}

	// Send the Z probe value
	int v0 = platform->ZProbe();
	int v1, v2;
	switch (platform->GetZProbeSecondaryValues(v1, v2))
	{
		case 1:
			response->catf(",\"probe\":\"%d (%d)\"", v0, v1);
			break;
		case 2:
			response->catf(",\"probe\":\"%d (%d, %d)\"", v0, v1, v2);
			break;
		default:
			response->catf(",\"probe\":\"%d\"", v0);
			break;
	}

	// Send the fan0 settings (for PanelDue firmware 1.13)
	response->catf(",\"fanPercent\":[%.02f,%.02f]", platform->GetFanValue(0) * 100.0, platform->GetFanValue(1) * 100.0);

	// Send fan RPM value
	response->catf(",\"fanRPM\":%u", static_cast<unsigned int>(platform->GetFanRPM()));

	// Send the home state. To keep the messages short, we send 1 for homed and 0 for not homed, instead of true and false.
	if (type != 0)
	{
		response->catf(",\"homed\":[%d,%d,%d]",
				(gc->GetAxisIsHomed(0)) ? 1 : 0,
				(gc->GetAxisIsHomed(1)) ? 1 : 0,
				(gc->GetAxisIsHomed(2)) ? 1 : 0);
	}
	else
	{
		response->catf(",\"hx\":%d,\"hy\":%d,\"hz\":%d",
				(gc->GetAxisIsHomed(0)) ? 1 : 0,
				(gc->GetAxisIsHomed(1)) ? 1 : 0,
				(gc->GetAxisIsHomed(2)) ? 1 : 0);
	}

	if (printMonitor->IsPrinting())
	{
		// Send the fraction printed
		response->catf(",\"fraction_printed\":%.4f", max<float>(0.0, gc->FractionOfFilePrinted()));
	}

	response->cat(",\"message\":");
	response->EncodeString(message, ARRAY_SIZE(message), false);

	if (type < 2)
	{
		response->catf(",\"buff\":%u", webserver->GetGCodeBufferSpace(WebSource::HTTP));	// send the amount of buffer space available for gcodes
	}
	else if (type == 2)
	{
		if (printMonitor->IsPrinting())
		{
			// Send estimated times left based on file progress, filament usage, and layers
			response->catf(",\"timesLeft\":[%.1f,%.1f,%.1f]",
					printMonitor->EstimateTimeLeft(fileBased),
					printMonitor->EstimateTimeLeft(filamentBased),
					printMonitor->EstimateTimeLeft(layerBased));
		}
	}
	else if (type == 3)
	{
		// Add the static fields. For now this is just the machine name, but other fields could be added e.g. axis lengths.
		response->cat(",\"myName\":");
		response->EncodeString(myName, ARRAY_SIZE(myName), false);
	}

	int auxSeq = (int)gCodes->GetAuxSeq();
	if (type < 2 || (seq != -1 && auxSeq != seq))
	{
		// Send the response to the last command. Do this last
		response->catf(",\"seq\":%u,\"resp\":", auxSeq);						// send the response sequence number

		// Send the JSON response
		response->EncodeReply(gCodes->GetAuxGCodeReply(), true);				// also releases the OutputBuffer chain
	}
	response->cat("}");

	return response;
}

// Copy some parameter text, stopping at the first control character or when the destination buffer is full, and removing trailing spaces
void RepRap::CopyParameterText(const char* src, char *dst, size_t length)
{
	size_t i;
	for (i = 0; i + 1 < length && src[i] >= ' '; ++i)
	{
		dst[i] = src[i];
	}
	// Remove any trailing spaces
	while (i > 0 && dst[i - 1] == ' ')
	{
		--i;
	}
	dst[i] = 0;
}

// Get the list of files in the specified directory in JSON format
OutputBuffer *RepRap::GetFilesResponse(const char *dir, bool flagsDirs)
{
	// Need something to write to...
	OutputBuffer *response;
	if (!OutputBuffer::Allocate(response))
	{
		return nullptr;
	}

	response->copy("{\"dir\":");
	response->EncodeString(dir, strlen(dir), false);
	response->cat(",\"files\":[");

	FileInfo fileInfo;
	bool firstFile = true;
	bool gotFile = platform->GetMassStorage()->FindFirst(dir, fileInfo);
	size_t bytesLeft = OutputBuffer::GetBytesLeft(response);	// don't write more bytes than we can
	char filename[FILENAME_LENGTH];
	filename[0] = '*';
	const char *fname;

	while (gotFile)
	{
		// Get the long filename if possible
		if (flagsDirs && fileInfo.isDirectory)
		{
			strncpy(filename + sizeof(char), fileInfo.fileName, FILENAME_LENGTH - 1);
			filename[FILENAME_LENGTH - 1] = 0;
			fname = filename;
		}
		else
		{
			fname = fileInfo.fileName;
		}

		// Make sure we can end this response properly
		if (bytesLeft < strlen(fname) * 2 + 4)
		{
			// No more space available - stop here
			break;
		}

		// Write separator and filename
		if (!firstFile)
		{
			bytesLeft -= response->cat(',');
		}
		bytesLeft -= response->EncodeString(fname, FILENAME_LENGTH, false);

		firstFile = false;
		gotFile = platform->GetMassStorage()->FindNext(fileInfo);
	}
	response->cat("]}");

	return response;
}

void RepRap::Beep(int freq, int ms)
{
	if (gCodes->HaveAux())
	{
		// If there is an AUX device present, make it beep. This should be eventually
		// removed when PanelDue can deal with new-style status responses.
		platform->Beep(freq, ms);
	}
	else
	{
		// Otherwise queue it until the webserver can process it
		beepFrequency = freq;
		beepDuration = ms;
	}
}

void RepRap::SetMessage(const char *msg)
{
	strncpy(message, msg, SHORT_STRING_LENGTH);
	message[SHORT_STRING_LENGTH - 1] = 0;
}

char RepRap::GetStatusCharacter() const
{
	if (processingConfig)
	{
		// Reading the configuration file
		return 'C';
	}
	if (gCodes->IsFlashing())
	{
		// Flashing a new firmware binary
		return 'F';
	}
	if (IsStopped())
	{
		// Halted
		return 'H';
	}
	if (gCodes->IsPausing())
	{
		// Pausing / Decelerating
		return 'D';
	}
	if (gCodes->IsResuming())
	{
		// Resuming
		return 'R';
	}
	if (gCodes->IsPaused())
	{
		// Paused / Stopped
		return 'S';
	}
	if (printMonitor->IsPrinting())
	{
		// Printing
		return 'P';
	}
	if (gCodes->DoingFileMacro() || !move->NoLiveMovement())
	{
		// Busy
		return 'B';
	}
	// Idle
	return 'I';
}

bool RepRap::NoPasswordSet() const
{
	return (!password[0] || StringEquals(password, DEFAULT_PASSWORD));
}

bool RepRap::CheckPassword(const char *pw) const
{
	return StringEquals(pw, password);
}

void RepRap::SetPassword(const char* pw)
{
	// Users sometimes put a tab character between the password and the comment, so allow for this
	CopyParameterText(pw, password, ARRAY_SIZE(password));
}

const char *RepRap::GetName() const
{
	return myName;
}

void RepRap::SetName(const char* nm)
{
	// Users sometimes put a tab character between the machine name and the comment, so allow for this
	CopyParameterText(nm, myName, ARRAY_SIZE(myName));

	// Set new DHCP hostname
	network->SetHostname(myName);
}

// Given that we want to extrude/etract the specified extruder drives, check if they are allowed.
// For each disallowed one, log an error to report later and return a bit in the bitmap.
// This may be called by an ISR!
unsigned int RepRap::GetProhibitedExtruderMovements(unsigned int extrusions, unsigned int retractions)
{
	unsigned int result = 0;
	Tool *tool = toolList;
	while (tool != nullptr)
	{
		for(size_t driveNum = 0; driveNum < tool->DriveCount(); driveNum++)
		{
			const int extruderDrive = tool->Drive(driveNum);
			unsigned int mask = 1 << extruderDrive;
			if (extrusions & mask)
			{
				if (!tool->ToolCanDrive(true))
				{
					result |= mask;
				}
			}
			else if (retractions & (1 << extruderDrive))
			{
				if (!tool->ToolCanDrive(false))
				{
					result |= mask;
				}
			}
		}

		tool = tool->Next();
	}
	return result;
}

// vim: ts=4:sw=4
