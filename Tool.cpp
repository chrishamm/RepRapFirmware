/****************************************************************************************************

RepRapFirmware - Tool

This class implements a tool in the RepRap machine, usually (though not necessarily) an extruder.

Tools may have zero or more drives associated with them and zero or more heaters.  There are a fixed number
of tools in a given RepRap, with fixed heaters and drives.  All this is specified on reboot, and cannot
be altered dynamically.  This restriction may be lifted in the future.  Tool descriptions are stored in
GCode macros that are loaded on reboot.

-----------------------------------------------------------------------------------------------------

Version 0.1

Created on: Apr 11, 2014

Adrian Bowyer
RepRap Professional Ltd
http://reprappro.com

Licence: GPL

****************************************************************************************************/

#include "RepRapFirmware.h"

Tool::Tool(int toolNumber, long d[], int dCount, long h[], int hCount)
{
	myNumber = toolNumber;
	next = nullptr;
	active = false;
	driveCount = dCount;
	heaterCount = hCount;
	heaterFault = false;
	mixing = false;
	displayColdExtrudeWarning = false;

	for(size_t axis = 0; axis < AXES; axis++)
	{
		offset[axis] = 0.0;
	}

	if (driveCount > 0)
	{
		if (driveCount > DRIVES - AXES)
		{
			reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Tool creation: Attempt to use more drives than there are in the RepRap...");
			driveCount = 0;
			heaterCount = 0;
			return;
		}

		drives = new int[driveCount];
		mix = new float[driveCount];
		float r = 1.0/(float)driveCount;

		for(size_t drive = 0; drive < driveCount; drive++)
		{
			drives[drive] = d[drive];
			mix[drive] = r;
		}
	}

	if (heaterCount > 0)
	{
		if (heaterCount > HEATERS)
		{
			reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Tool creation: Attempt to use more heaters than there are in the RepRap...");
			driveCount = 0;
			heaterCount = 0;
			return;
		}

		heaters = new int[heaterCount];
		activeTemperatures = new float[heaterCount];
		standbyTemperatures = new float[heaterCount];

		for(size_t heater = 0; heater < heaterCount; heater++)
		{
			heaters[heater] = h[heater];
			activeTemperatures[heater] = ABS_ZERO;
			standbyTemperatures[heater] = ABS_ZERO;
		}
	}
}

void Tool::Print(StringRef& reply)
{
	reply.printf("Tool %d - drives: ", myNumber);
	char comma = ',';
	for(size_t drive = 0; drive < driveCount; drive++)
	{
		if(drive >= driveCount - 1)
		{
			comma = ';';
		}
		reply.catf("%d%c", drives[drive], comma);
	}

	reply.catf("heaters (active/standby temps): ");
	comma = ',';
	for(size_t heater = 0; heater < heaterCount; heater++)
	{
			if(heater >= heaterCount - 1)
			{
				comma = ';';
			}
			reply.catf("%d (%.1f/%.1f)%c", heaters[heater],
					activeTemperatures[heater], standbyTemperatures[heater], comma);
	}

	reply.catf(" status: %s", active ? "selected" : "standby");
}

float Tool::MaxFeedrate() const
{
	if(driveCount <= 0)
	{
		reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Attempt to get maximum feedrate for a tool with no drives.\n");
		return 1.0;
	}
	float result = 0.0;
	for(size_t d = 0; d < driveCount; d++)
	{
		float mf = reprap.GetPlatform()->MaxFeedrate(drives[d] + AXES);
		if(mf > result)
		{
			result = mf;
		}
	}
	return result;
}

float Tool::InstantDv() const
{
	if (driveCount <= 0)
	{
		reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Attempt to get InstantDv for a tool with no drives.\n");
		return 1.0;
	}

	float result = FLT_MAX;
	for(size_t d = 0; d < driveCount; d++)
	{
		float idv = reprap.GetPlatform()->ActualInstantDv(drives[d] + AXES);
		if (idv < result)
		{
			result = idv;
		}
	}
	return result;
}

// Add a tool to the end of the linked list.
// (We must already be in it.)

void Tool::AddTool(Tool* tool)
{
	Tool* t = this;
	Tool* last = this;		// initialised to suppress spurious compiler warning
	while (t != nullptr)
	{
		last = t;
		t = t->Next();
	}
	tool->next = nullptr; // Defensive...
	last->next = tool;
}

// There is a temperature fault on a heater.
// Disable all tools using that heater.
// This function must be called for the first
// entry in the linked list.

void Tool::FlagTemperatureFault(int8_t heater)
{
	Tool* n = this;
	while (n != nullptr)
	{
		n->SetTemperatureFault(heater);
		n = n->Next();
	}
}

void Tool::ClearTemperatureFault(int8_t heater)
{
	Tool* n = this;
	while (n != nullptr)
	{
		n->ResetTemperatureFault(heater);
		n = n->Next();
	}
}

void Tool::SetTemperatureFault(int8_t dudHeater)
{
	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		if (dudHeater == heaters[heater])
		{
			heaterFault = true;
			return;
		}
	}
}

void Tool::ResetTemperatureFault(int8_t wasDudHeater)
{
	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		if (wasDudHeater == heaters[heater])
		{
			heaterFault = false;
			return;
		}
	}
}

bool Tool::AllHeatersAtHighTemperature(bool forExtrusion) const
{
	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		const float temperature = reprap.GetHeat()->GetTemperature(heaters[heater]);
		if (temperature < HOT_ENOUGH_TO_EXTRUDE && forExtrusion)
		{
			return false;
		}
		else if (temperature < HOT_ENOUGH_TO_RETRACT)
		{
			return false;
		}
	}
	return true;
}

void Tool::Activate(Tool* currentlyActive)
{
	if (active)
		return;

	if (currentlyActive != nullptr && currentlyActive != this)
	{
		currentlyActive->Standby();
	}

	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		reprap.GetHeat()->SetActiveTemperature(heaters[heater], activeTemperatures[heater]);
		reprap.GetHeat()->SetStandbyTemperature(heaters[heater], standbyTemperatures[heater]);
		reprap.GetHeat()->Activate(heaters[heater]);
	}
	active = true;
}

void Tool::Standby()
{
	if (!active)
		return;

	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		reprap.GetHeat()->SetStandbyTemperature(heaters[heater], standbyTemperatures[heater]);
		reprap.GetHeat()->Standby(heaters[heater]);
	}
	active = false;
}

void Tool::SetVariables(const float* standby, const float* active)
{
	bool toolActive = (reprap.GetCurrentTool() == this);

	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		if (active[heater] < NEARLY_ABS_ZERO && standby[heater] < NEARLY_ABS_ZERO)
		{
			// Temperatures close to ABS_ZERO turn off all associated heaters
			reprap.GetHeat()->SwitchOff(heaters[heater]);
		}
		else
		{
			const float temperatureLimit = reprap.GetPlatform()->GetTemperatureLimit();
			if (active[heater] < temperatureLimit)
			{
				activeTemperatures[heater] = active[heater];
				reprap.GetHeat()->SetActiveTemperature(heaters[heater], activeTemperatures[heater]);
			}

			if (standby[heater] < temperatureLimit)
			{
				standbyTemperatures[heater] = standby[heater];
				reprap.GetHeat()->SetStandbyTemperature(heaters[heater], standbyTemperatures[heater]);
			}

			if (toolActive)
			{
				// Must do this in case the heater was switched off before
				reprap.GetHeat()->Activate(heaters[heater]);
			}
		}
	}
}

void Tool::GetVariables(float* standby, float* active) const
{
	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		active[heater] = activeTemperatures[heater];
		standby[heater] = standbyTemperatures[heater];
	}
}

// May be called from ISR
bool Tool::ToolCanDrive(bool extrude)
{
	if (!heaterFault && AllHeatersAtHighTemperature(extrude))
	{
		return true;
	}

	displayColdExtrudeWarning = true;
	return false;
}

// Update the number of active drives and extruders in use to reflect what this tool uses
void Tool::UpdateExtruderAndHeaterCount(uint16_t &numExtruders, uint16_t &numHeaters) const
{
	for(size_t drive = 0; drive < driveCount; drive++)
	{
		if (drives[drive] >= numExtruders)
		{
			numExtruders = drives[drive] + 1;
		}
	}

	for(size_t heater = 0; heater < heaterCount; heater++)
	{
		if (heaters[heater] != reprap.GetHeat()->GetBedHeater() && heaters[heater] >= numHeaters)
		{
			numHeaters = heaters[heater] + 1;
		}
	}
}

bool Tool::DisplayColdExtrudeWarning()
{
	bool result = displayColdExtrudeWarning;
	displayColdExtrudeWarning = false;
	return result;
}

// vim: ts=4:sw=4
