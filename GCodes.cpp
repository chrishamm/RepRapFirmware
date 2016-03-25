/****************************************************************************************************

 RepRapFirmware - G Codes

 This class interprets G Codes from one or more sources, and calls the functions in Move, Heat etc
 that drive the machine to do what the G Codes command.

 Most of the functions in here are designed not to wait, and they return a boolean.  When you want them to do
 something, you call them.  If they return false, the machine can't do what you want yet.  So you go away
 and do something else.  Then you try again.  If they return true, the thing you wanted done has been done.

 -----------------------------------------------------------------------------------------------------

 Version 0.1

 13 February 2013

 Adrian Bowyer
 RepRap Professional Ltd
 http://reprappro.com

 Licence: GPL

 ****************************************************************************************************/

#include "RepRapFirmware.h"

#define DEGREE_SYMBOL        "\xC2\xB0"                              // degree-symbol encoding in UTF8

GCodes::GCodes(Platform* p, Webserver* w) :
	platform(p), active(false), webserver(w), stackPointer(0), fileStackPointer(0), isFlashing(false)
{
	httpGCode = new GCodeBuffer(platform, "http: ");
	telnetGCode = new GCodeBuffer(platform, "telnet: ");
	fileGCode = new GCodeBuffer(platform, "file: ");
	serialGCode = new GCodeBuffer(platform, "serial: ");
	auxGCode = new GCodeBuffer(platform, "aux: ");
	fileMacroGCode = new GCodeBuffer(platform, "macro: ");
	queueGCode = new GCodeBuffer(platform, "queue: ");
	auxGCodeReply = nullptr;
}

void GCodes::Exit()
{
	platform->Message(HOST_MESSAGE, "GCodes class exited.\n");
	active = false;
}

void GCodes::Init()
{
	Reset();
	drivesRelative = true;
	axesRelative = false;
	ARRAY_INIT(axisLetters, AXIS_LETTERS);
	distanceScale = 1.0;
	for(size_t extruder = 0; extruder < DRIVES - AXES; extruder++)
	{
		lastExtruderPosition[extruder] = 0.0;
	}
	eofStringCounter = 0;
	homing = false;
	homeX = false;
	homeY = false;
	homeZ = false;
	settingBedEquationWithProbe = false;
	offSetSet = false;
	zProbesSet = false;
	active = true;
	longWait = platform->Time();
	dwellTime = longWait;
	limitAxes = true;
	for(size_t axis = 0; axis < AXES; axis++)
	{
		axisIsHomed[axis] = false;
		axisScaleFactors[axis] = 1.0;
	}
	toolChangeSequence = 0;
	for(size_t fan = 0; fan < NUM_FANS; fan++)
	{
		pausedFanValues[fan] = 0.0;
	}
	internalCodeQueue = nullptr;
	releasedQueueItems = nullptr;
	for(size_t i = 0; i < CODE_QUEUE_LENGTH; i++)
	{
		releasedQueueItems = new CodeQueueItem(releasedQueueItems);
	}
}

// This is called from Init and when doing an emergency stop
void GCodes::Reset()
{
	httpGCode->Init();
	telnetGCode->Init();
	fileGCode->Init();
	serialGCode->Init();
	auxGCode->Init();
	fileMacroGCode->Init();
	queueGCode->Init();
	moveAvailable = false;
	totalMoves = 0;
	movesCompleted = 0;
	fileBeingPrinted.Close();
	fileToPrint.Close();
	fileBeingWritten = nullptr;
	endStopsToCheck = 0;
	doingFileMacro = returningFromMacro = false;
	macroSourceGCodeBuffer = waitingForMoveGCodeBuffer = nullptr;
	pauseStatus = PauseStatus::NotPaused;
	for(size_t drive = 0; drive < DRIVES; drive++)
	{
		pauseCoordinates[drive] = 0.0;
	}
	pauseCoordinates[DRIVES] = DEFAULT_FEEDRATE;
	doPauseMacro = false;
	fractionOfFilePrinted = -1.0;
	dwellWaiting = false;
	stackPointer = fileStackPointer = 0;
	lastMacroPosition = 0;
	probeCount = 0;
	cannedCycleMoveCount = 0;
	cannedCycleMoveQueued = false;
	auxDetected = false;
	OutputBuffer::ReleaseAll(auxGCodeReply);
	auxGCodeReply = nullptr;
	auxSeq = 0;
	simulating = false;
	simulationTime = 0.0;
	filePos = moveFilePos = NO_FILE_POSITION;

	isHashing = false;
}

void GCodes::Spin()
{
	if (!active)
	{
		return;
	}

	// Check each of the sources of G Codes (macro, http, telnet, aux, serial, queue and file) to
	// see if they are finished in order to feed them new codes.

	// Macro
	if (doingFileMacro && !fileMacroGCode->Active() && fileBeingPrinted.IsLive())
	{
		size_t i = 0;
		do {
			char b;
			if (fileBeingPrinted.Read(b))
			{
				if (fileMacroGCode->Put(b))
				{
					// Will be acted upon later
					break;
				}
			}
			else
			{
				// Cannot read any more from the file, so try to finish the last G-Code
				if (fileMacroGCode->Put('\n'))
				{
					// Will be acted upon later
					break;
				}

				// Read from file has finished, close it and pop the stack
				if (!fileMacroGCode->Active() && Pop())
				{
					fileStack[stackPointer + 1].Close();
					if (doingFileMacro)
					{
						// Keep track of the current file position in case the running G-code starts another macro file
						lastMacroPosition = fileBeingPrinted.Position();
					}
					returningFromMacro = true;
				}
				break;
			}
		} while (++i < GCODE_LENGTH);
	}

	// HTTP
	if (!httpGCode->Active() && webserver->GCodeAvailable(WebSource::HTTP))
	{
		size_t i = 0;
		do {
			char b = webserver->ReadGCode(WebSource::HTTP);
			if (httpGCode->Put(b))
			{
				// Will be acted upon later
				break;
			}
		} while (++i < GCODE_LENGTH && webserver->GCodeAvailable(WebSource::HTTP));
	}

	// Telnet
	if (!telnetGCode->Active() && webserver->GCodeAvailable(WebSource::Telnet))
	{
		size_t i = 0;
		do {
			char b = webserver->ReadGCode(WebSource::Telnet);
			if (telnetGCode->Put(b))
			{
				// Will be acted upon later
				break;
			}
		} while (++i < GCODE_LENGTH && webserver->GCodeAvailable(WebSource::Telnet));
	}

	// Serial (USB)
	if (serialGCode->WritingFileDirectory() == platform->GetWebDir())
	{
		// First check the special case of uploading the reprap.htm file
		size_t i = 0;
		while (i++ < GCODE_LENGTH && platform->GCodeAvailable(SerialSource::USB))
		{
			char b = platform->ReadFromSource(SerialSource::USB);
			WriteHTMLToFile(b, serialGCode);
		}
	}
	else if (!serialGCode->Active() && platform->GCodeAvailable(SerialSource::USB))
	{
		// Otherwise just deal in general with incoming bytes from the serial interface
		size_t i = 0;
		do {
			char b = platform->ReadFromSource(SerialSource::USB);
			if (serialGCode->Put(b))
			{
				if (serialGCode->WritingFileDirectory() != nullptr)
				{
					WriteGCodeToFile(serialGCode);
					serialGCode->SetFinished(true);
				}
				// Else it will be acted upon later
				break;
			}
		} while (++i < GCODE_LENGTH && platform->GCodeAvailable(SerialSource::USB));
	}

	// AUX
	if (!auxGCode->Active() && platform->GCodeAvailable(SerialSource::AUX))
	{
		size_t i = 0;
		do {
			char b = platform->ReadFromSource(SerialSource::AUX);
			if (auxGCode->Put(b))
			{
				// Will be acted upon later
				auxDetected = true;
				break;
			}
		} while (++i < GCODE_LENGTH && platform->GCodeAvailable(SerialSource::AUX));
	}

	// Code Queue
	if (!queueGCode->Active() && internalCodeQueue != nullptr && internalCodeQueue->ExecuteAtMove() <= movesCompleted)
	{
		// Process another queued G-code
		queueGCodeSource = internalCodeQueue->GetSource();
		if (!queueGCode->Put(internalCodeQueue->GetCode(), internalCodeQueue->GetCodeLength()))
		{
			queueGCode->Put('\n');
		}

		// Release this item again
		CodeQueueItem *temp = internalCodeQueue;
		internalCodeQueue = internalCodeQueue->Next();
		temp->SetNext(releasedQueueItems);
		releasedQueueItems = temp;
	}

	// File
	if (!DoingFileMacro() && !fileGCode->Active() && fileBeingPrinted.IsLive())
	{
		size_t i = 0;
		do {
			char b;
			if (fileBeingPrinted.Read(b))
			{
				if (fileGCode->Put(b))
				{
					// Will be acted upon later
					break;
				}
			}
			else
			{
				// Cannot read any more from the file, so try to finish the last G-Code
				if (fileGCode->Put('\n'))
				{
					// Will be acted upon later
					break;
				}

				// Read from file has finished, close it and pop the stack
				if (!fileGCode->Active() && internalCodeQueue == nullptr && Pop())
				{
					fileStack[stackPointer + 1].Close();
					reprap.GetPrintMonitor()->StoppedPrint();

					if (platform->Emulating() == marlin)
					{
						// If we're emulating Marlin, send a notification to Pronterface
						HandleReply(fileGCode, false, "Done printing file\n");
					}
				}
				break;
			}
		} while (++i < GCODE_LENGTH);
	}


	// Now run the G-Code buffers...

	if (fileMacroGCode->Active())
	{
		const uint8_t lastStackPointer = stackPointer;
		bool finished = ActOnCode(fileMacroGCode, false);
		if ((stackPointer > lastStackPointer) && (fileStack[lastStackPointer] != fileBeingPrinted))
		{
			// We must finish the current G-Code for now if another nested macro is started
			finished = true;
		}
		else if (finished && fileBeingPrinted.IsLive())
		{
			// Else keep track of the file position. We may need it again when another nested macro is started
			lastMacroPosition = fileBeingPrinted.Position();
		}
		fileMacroGCode->SetFinished(finished);
	}
	if (httpGCode->Active())
	{
		// Note: Direct web-printing has been dropped, so it's safe to execute web codes immediately
		httpGCode->SetFinished(ActOnCode(httpGCode));
	}
	if (telnetGCode->Active())
	{
		// Telnet may be used via Pronterface, so make it behave the same way as the serial GCode
		telnetGCode->SetFinished(ActOnCode(telnetGCode, IsPaused()));
	}
	if (serialGCode->Active())
	{
		// We want codes from the serial interface to be queued unless the print has been paused
		serialGCode->SetFinished(ActOnCode(serialGCode, IsPaused()));
	}
	if (auxGCode->Active())
	{
		auxGCode->SetFinished(ActOnCode(auxGCode));
	}
	if (queueGCode->Active())
	{
		queueGCode->SetToolNumberAdjust(queueGCodeSource->GetToolNumberAdjust());
		queueGCode->SetFinished(ActOnCode(queueGCode));
	}
	if (fileGCode->Active() && (fileStackPointer == stackPointer || macroSourceGCodeBuffer == fileGCode))
	{
		fileGCode->SetFinished(ActOnCode(fileGCode, false));
		fileStackPointer = stackPointer;
	}

	platform->ClassReport(longWait);
}

void GCodes::Diagnostics()
{
	platform->Message(GENERIC_MESSAGE, "GCodes Diagnostics:\n");
	platform->MessageF(GENERIC_MESSAGE, "Move available? %s\n", moveAvailable ? "yes" : "no");
	platform->MessageF(GENERIC_MESSAGE, "Internal code queue is %s\n", (internalCodeQueue == nullptr) ? "empty." : "not empty:");
	if (internalCodeQueue != nullptr)
	{
		platform->MessageF(GENERIC_MESSAGE, "Total moves: %d, moves completed: %d\n", totalMoves, movesCompleted);
		size_t queueLength = 0;
		CodeQueueItem *item = internalCodeQueue;
		do {
			queueLength++;
			platform->MessageF(GENERIC_MESSAGE, "Queued '%s' for move %d\n", item->GetCode(), item->ExecuteAtMove());
		} while ((item = item->Next()) != nullptr);
		platform->MessageF(GENERIC_MESSAGE, "%d of %d codes have been queued.\n", queueLength, CODE_QUEUE_LENGTH);
	}
	platform->MessageF(GENERIC_MESSAGE, "Stack pointer: %u of %u\n", stackPointer, STACK);
}

// The wait till everything's done function.  If you need the machine to
// be idle before you do something (for example homing an axis, or shutting down) call this
// until it returns true.  As a side-effect it loads moveBuffer with the last
// position and feedrate for you.
bool GCodes::AllMovesAreFinishedAndMoveBufferIsLoaded()
{
	// Last one gone?
	if (moveAvailable)
		return false;

	// Wait for all the queued moves to stop so we get the actual last position and feedrate
	if (!reprap.GetMove()->AllMovesAreFinished())
		return false;
	reprap.GetMove()->ResumeMoving();

	// Load the last position
	reprap.GetMove()->GetCurrentUserPosition(moveBuffer, 0);
	return true;
}

// Save (some of) the state of the machine for recovery in the future.
// Call repeatedly till it returns true.
bool GCodes::Push()
{
	if (stackPointer >= STACK)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Push() stack overflow!\n");
		return true;
	}

	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		return false;

	drivesRelativeStack[stackPointer] = drivesRelative;
	axesRelativeStack[stackPointer] = axesRelative;
	feedrateStack[stackPointer] = moveBuffer[DRIVES];
	for(size_t extruder = 0; extruder < DRIVES - AXES; extruder++)
	{
		extruderPositionStack[stackPointer][extruder] = lastExtruderPosition[extruder];
	}
	doingFileMacroStack[stackPointer] = doingFileMacro;
	fileStack[stackPointer].CopyFrom(fileBeingPrinted);
	stackPointer++;

	return true;
}

// Recover a saved state.  Call repeatedly till it returns true.
bool GCodes::Pop()
{
	if (stackPointer < 1)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Pop() stack underflow!\n");
		return true;
	}

	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		return false;

	stackPointer--;
	drivesRelative = drivesRelativeStack[stackPointer];
	axesRelative = axesRelativeStack[stackPointer];
	moveBuffer[DRIVES] = feedrateStack[stackPointer];
	reprap.GetMove()->SetFeedrate(feedrateStack[stackPointer]);
	doingFileMacro = doingFileMacroStack[stackPointer];
	for(size_t extruder = 0; extruder < DRIVES - AXES; extruder++)
	{
		lastExtruderPosition[extruder] = extruderPositionStack[stackPointer][extruder];
	}
	fileBeingPrinted.MoveFrom(fileStack[stackPointer]);
	endStopsToCheck = 0;

	return true;
}

// Move expects all axis movements to be absolute, and all
// extruder drive moves to be relative.  This function serves that.
// If applyLimits is true and we have homed the relevant axes, then we don't allow movement beyond the bed.
bool GCodes::LoadMoveBufferFromGCode(GCodeBuffer *gb, bool doingG92, bool applyLimits)
{
	// Zero every extruder drive as some drives may not be changed
	for(size_t drive = AXES; drive < DRIVES; drive++)
	{
		moveBuffer[drive] = 0.0;
	}

	// Deal with feed rate
	if (gb->Seen(FEEDRATE_LETTER))
	{
		moveBuffer[DRIVES] = gb->GetFValue() * distanceScale * SECONDS_TO_MINUTES; // G Code feedrates are in mm/minute; we need mm/sec
	}

	// First do extrusion, and check, if we are extruding, that we have a tool to extrude with
	Tool* tool = reprap.GetCurrentTool();

	if (gb->Seen(EXTRUDE_LETTER))
	{
		if (tool == nullptr)
		{
			if (reprap.ToolWarningsAllowed())
			{
				platform->Message(GENERIC_MESSAGE, "Error: Attempting to extrude with no tool selected.\n");
			}
			return false;
		}

		size_t eMoveCount = tool->DriveCount();
		if (eMoveCount > 0)
		{
			float eMovement[DRIVES-AXES];
			if (tool->Mixing())
			{
				float length = gb->GetFValue();
				for(size_t drive = 0; drive < tool->DriveCount(); drive++)
				{
					eMovement[drive] = length * tool->GetMix()[drive];
				}
			}
			else
			{
				size_t mc = eMoveCount;
				gb->GetFloatArray(eMovement, mc);
				if (mc != eMoveCount)
				{
					platform->MessageF(GENERIC_MESSAGE, "Error: Wrong number of extruder drives for the selected tool: %s\n", gb->Buffer());
					return false;
				}
			}

			// Set the drive values for this tool.
			for(size_t eDrive = 0; eDrive < eMoveCount; eDrive++)
			{
				size_t drive = tool->Drive(eDrive);
				float moveArg = eMovement[eDrive] * distanceScale;
				if (doingG92)
				{
					moveBuffer[drive + AXES] = moveArg;
					lastExtruderPosition[drive] = moveArg;
				}
				else
				{
					if (drivesRelative)
					{
						moveBuffer[drive + AXES] = moveArg;
						lastExtruderPosition[drive] += moveArg;
					}
					else
					{
						moveBuffer[drive + AXES] = moveArg - lastExtruderPosition[drive];
						lastExtruderPosition[drive] = moveArg;
					}
				}
			}
		}
	}

	// Now the movement axes
	bool doingResumeMove = false;
	if (gb->Seen('R') && gb->GetIValue() > 0)
	{
		if (IsRunning())
		{
			platform->Message(GENERIC_MESSAGE, "Error: Resume moves are only allowed while the print is paused!\n");
			return false;
		}
		doingResumeMove = true;
	}

	for(size_t axis = 0; axis < AXES; axis++)
	{
		if (gb->Seen(axisLetters[axis]))
		{
			float moveArg = gb->GetFValue() * distanceScale;

			if (reprap.GetMove()->IsDeltaMode())
			{
				// Perform axis scale compensation for Delta configurations
				moveArg *= axisScaleFactors[axis];
			}

			if (doingG92)
			{
				axisIsHomed[axis] = true;		// doing a G92 defines the absolute axis position
			}
			else
			{
				if (doingResumeMove)
				{
					moveArg += pauseCoordinates[axis];
				}
				else if (axesRelative)
				{
					moveArg += moveBuffer[axis];
				}
				else if (tool != nullptr)
				{
					moveArg -= tool->GetOffset()[axis];		// adjust requested position to compensate for tool offset
				}

				// If on a Cartesian printer and applying limits, limit all axes
				if (applyLimits && axisIsHomed[axis] && !(reprap.GetMove()->IsDeltaMode() || reprap.GetRoland()->Active()))
				{
					if (moveArg < platform->AxisMinimum(axis))
					{
						moveArg = platform->AxisMinimum(axis);
					}
					else if (moveArg > platform->AxisMaximum(axis))
					{
						moveArg = platform->AxisMaximum(axis);
					}
				}
			}
			moveBuffer[axis] = moveArg;
		}
		else if (doingResumeMove)
		{
			moveBuffer[axis] = pauseCoordinates[axis];
		}
	}

	// If axes have been homed on a delta printer and this isn't a homing move, check for movements outside limits.
	// Skip this check if axes have not been homed, so that extruder-only moved are allowed before homing
	if (applyLimits && reprap.GetMove()->IsDeltaMode() && AllAxesAreHomed())
	{
		// Constrain the move to be within the build radius
	 	float diagonalSquared = fsquare(moveBuffer[X_AXIS]) + fsquare(moveBuffer[Y_AXIS]);
		if (diagonalSquared > reprap.GetMove()->GetDeltaParams().GetPrintRadiusSquared())
		{
			float factor = sqrtf(reprap.GetMove()->GetDeltaParams().GetPrintRadiusSquared() / diagonalSquared);
			moveBuffer[X_AXIS] *= factor;
			moveBuffer[Y_AXIS] *= factor;
		}

		// Constrain the end height of the move to be no greater than the homed height and no lower than -0.2mm
		moveBuffer[Z_AXIS] = max<float>(platform->AxisMinimum(Z_AXIS),
			min<float>(moveBuffer[Z_AXIS], reprap.GetMove()->GetDeltaParams().GetHomedHeight()));
	}

	return true;
}

// This function is called for a G Code that makes a move.
// If the Move class can't receive the move (i.e. things have to wait), return 0.
// If we have queued the move and the caller doesn't need to wait for it to complete, return 1.
// If we need to wait for the move to complete before doing another one (because endstops are checked in this move), return 2.
int GCodes::SetUpMove(GCodeBuffer *gb, StringRef& reply)
{
	// Last one gone yet?
	if (moveAvailable)
	{
		return 0;
	}

	// Check to see if the move is a 'homing' move that endstops are checked on.
	endStopsToCheck = 0;
	moveType = 0;
	if (gb->Seen('S'))
	{
		int ival = gb->GetIValue();
		if (ival == 1 || ival == 2)
		{
			moveType = ival;
		}

		if (ival == 1)
		{
			for (size_t i = 0; i < AXES; ++i)
			{
				if (gb->Seen(axisLetters[i]))
				{
					endStopsToCheck |= (1 << i);
				}
			}
		}
	}

	if (reprap.GetMove()->IsDeltaMode())
	{
		// Extra checks to avoid damaging delta printers
		if (moveType != 0 && !axesRelative)
		{
			// We have been asked to do a move without delta mapping on a delta machine, but the move is not relative.
			// This may be damaging and is almost certainly a user mistake, so ignore the move.
			reply.copy("Attempt to move the motors of a delta printer to absolute positions\n");
			return 1;
		}

		if (moveType == 0 && !AllAxesAreHomed())
		{
			// The user may be attempting to move a delta printer to an XYZ position before homing the axes
			// This may be damaging and is almost certainly a user mistake, so ignore the move. But allow extruder-only moves.
			if (gb->Seen(axisLetters[X_AXIS]) || gb->Seen(axisLetters[Y_AXIS]) || gb->Seen(axisLetters[Z_AXIS]))
			{
				reply.copy("Attempt to move the head of a delta printer before homing the towers\n");
				return 1;
			}
		}
	}

	// Load the last position and feed rate into moveBuffer
	if (reprap.GetRoland()->Active())
	{
		reprap.GetRoland()->GetCurrentRolandPosition(moveBuffer);
	}
	else
	{
		reprap.GetMove()->GetCurrentUserPosition(moveBuffer, moveType);
	}

	// Load the move buffer with either the absolute movement required or the relative movement required
	moveAvailable = LoadMoveBufferFromGCode(gb, false, limitAxes && moveType == 0);
	if (moveAvailable)
	{
		moveFilePos = (gb == fileGCode) ? filePos : NO_FILE_POSITION;
		//debugPrintf("Queue move pos %u\n", moveFilePos);
	}
	return (moveType != 0) ? 2 : 1;
}

// The Move class calls this function to find what to do next.
bool GCodes::ReadMove(float m[], EndstopChecks& ce, uint8_t& rMoveType, FilePosition& fPos)
{
	if (!moveAvailable)
	{
		return false;
	}

	for(size_t i = 0; i <= DRIVES; i++)			// 1 more for feedrate
	{
		m[i] = moveBuffer[i];
	}
	ce = endStopsToCheck;
	rMoveType = moveType;
	fPos = moveFilePos;
	ClearMove();
	return true;
}

void GCodes::ClearMove()
{
	moveAvailable = false;
	endStopsToCheck = 0;
	moveType = 0;
}

bool GCodes::DoFileMacro(const GCodeBuffer *gb, const char* fileName)
{
	// Can we run another macro file at this point?
	if (doingFileMacro && gb != fileMacroGCode)
	{
		return false;
	}

	// Are we returning from a macro?
	if (returningFromMacro)
	{
		// We can confirm this macro was called by a verified source, so make it return
		if (gb == fileMacroGCode || gb == macroSourceGCodeBuffer)
		{
			returningFromMacro = false;
			if (!doingFileMacro)
			{
				// Macro file has been processed, reset some values here
				macroSourceGCodeBuffer = nullptr;
				if (IsRunning())
				{
					// Don't reset this value if the print is paused - M0/M1/M24 do this
					fractionOfFilePrinted = -1.0;
				}
			}
			return true;
		}

		// This request was issued by another GCodeBuffer, so make it wait a bit longer
		return false;
	}

	// See if we can save the file progress and push some values on the stack
	if (!doingFileMacro && fileBeingPrinted.IsLive())
	{
		fractionOfFilePrinted = fileBeingPrinted.FractionRead();
	}

	if (!Push())
	{
		return false;
	}

	// Then check if we can open the file
	FileStore *f;
	if (StringStartsWith(fileName, FS_PREFIX))
	{
		// If the filename already provides a proper path, don't attempt to find it in either /sys or /macros
		f = platform->GetFileStore(fileName, false);
	}
	else
	{
		if (fileName[0] == '/')
		{
			// It's a file path from the root; only prepend the FS prefix
			f = platform->GetFileStore(FS_PREFIX, fileName, false);
		}
		else
		{
			// Does the file exist in /sys?
			const MassStorage *massStorage = platform->GetMassStorage();
			if (massStorage->FileExists(platform->GetSysDir(), fileName))
			{
				// Yes - open it
				f = platform->GetFileStore(platform->GetSysDir(), fileName, false);
			}
			else
			{
				// No - does it exist in /macros?
				if (massStorage->FileExists(platform->GetMacroDir(), fileName))
				{
					// Yes - open it
					f = platform->GetFileStore(platform->GetMacroDir(), fileName, false);
				}
				else
				{
					// No - report an error
					f = nullptr;
					platform->MessageF(GENERIC_MESSAGE, "Error: Macro file '%s' not found in neither /sys nor /macros!\n", fileName);
				}
			}
		}
	}

	if (f == nullptr)
	{
		Pop();
		return true;
	}
	fileBeingPrinted.Set(f);

	// Deal with nested macros
	if (gb == fileMacroGCode)
	{
		// Rewind back to the position where the last code started so it's called again later
		fileStack[stackPointer - 1].Seek(lastMacroPosition);
	}
	else
	{
		// Keep track of which GCodeBuffer initiated this macro file
		macroSourceGCodeBuffer = gb;
	}

	// Set some values so the macro file gets processed properly
	doingFileMacro = true;
	lastMacroPosition = 0;
	fileMacroGCode->Init();

	return false;
}

bool GCodes::FileMacroCyclesReturn()
{
	// Go up through the stack and close all macro files
	while (doingFileMacro) {
		if (fileBeingPrinted.IsLive())
		{
			fileBeingPrinted.Close();
		}

		if (!Pop())
		{
			return false;
		}
	}

	// Clean up when ready
	returningFromMacro = true;
	return true;
}

// To execute any move, call this until it returns true.
// moveToDo[] entries corresponding with false entries in action[] will
// be ignored.  Recall that moveToDo[DRIVES] should contain the feed rate
// you want (if action[DRIVES] is true).
bool GCodes::DoCannedCycleMove(EndstopChecks ce)
{
	// Is the move already running?

	if (cannedCycleMoveQueued)
	{ // Yes.
		if (!Pop()) // Wait for the move to finish then restore the state
		{
			return false;
		}
		cannedCycleMoveQueued = false;
		return true;
	}
	else
	{ // No.
		if (!Push()) // Wait for the RepRap to finish whatever it was doing, save it's state, and load moveBuffer[] with the current position.
		{
			return false;
		}

		for(size_t drive = 0; drive <= DRIVES; drive++)
		{
			if (activeDrive[drive])
			{
				moveBuffer[drive] = moveToDo[drive];
			}
		}
		endStopsToCheck = ce;
		cannedCycleMoveQueued = true;
		moveAvailable = true;
	}
	return false;
}

// This sets positions.  I.e. it handles G92.
bool GCodes::SetPositions(GCodeBuffer *gb)
{
	// Don't pause the machine if only extruder drives are being reset (DC, 2015-09-06).
	// This avoids blobs and seams when the gcode uses absolute E coordinates and periodically includes G92 E0.
	bool includingAxes = false;
	for (size_t drive = 0; drive < AXES; ++drive)
	{
		if (gb->Seen(axisLetters[drive]))
		{
			includingAxes = true;
			break;
		}
	}

	if (includingAxes)
	{
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
	}
	else if (moveAvailable)			// wait for previous move to be taken so that GetCurrentUserPosition returns the correct value
	{
		return false;
	}

	reprap.GetMove()->GetCurrentUserPosition(moveBuffer, 0);		// make sure move buffer is up to date
	bool ok = LoadMoveBufferFromGCode(gb, true, false);
	if (ok && includingAxes)
	{
		if (reprap.GetRoland()->Active())
		{
			for(size_t axis = 0; axis < AXES; axis++)
			{
				if (!reprap.GetRoland()->ProcessG92(moveBuffer[axis], axis))
				{
					return false;
				}
			}
		}

		SetPositions(moveBuffer);
	}

	return true;
}

void GCodes::SetPositions(float positionNow[DRIVES])
{
	// Transform the position so that e.g. if the user does G92 Z0,
	// the position we report (which gets inverse-transformed) really is Z=0 afterwards
	reprap.GetMove()->Transform(moveBuffer);
	reprap.GetMove()->SetLiveCoordinates(moveBuffer);
	reprap.GetMove()->SetPositions(moveBuffer);
}

// Offset the axes by the X, Y, and Z amounts in the M code in gb.  Say the machine is at [10, 20, 30] and
// the offsets specified are [8, 2, -5].  The machine will move to [18, 22, 25] and henceforth consider that point
// to be [10, 20, 30].
bool GCodes::OffsetAxes(GCodeBuffer* gb)
{
	if (!offSetSet)
	{
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			return false;

		for(size_t drive = 0; drive <= DRIVES; drive++)
		{
			if (drive < AXES || drive == DRIVES)
			{
				record[drive] = moveBuffer[drive];
				moveToDo[drive] = moveBuffer[drive];
			}
			else
			{
				record[drive] = 0.0;
				moveToDo[drive] = 0.0;
			}
			activeDrive[drive] = false;
		}

		for(size_t axis = 0; axis < AXES; axis++)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				moveToDo[axis] += gb->GetFValue();
				activeDrive[axis] = true;
			}
		}

		if (gb->Seen(FEEDRATE_LETTER)) // Has the user specified a feedrate?
		{
			moveToDo[DRIVES] = gb->GetFValue();
			activeDrive[DRIVES] = true;
		}

		offSetSet = true;
	}

	if (DoCannedCycleMove(0))
	{
		for (size_t drive = 0; drive <= DRIVES; drive++)
		{
			moveBuffer[drive] = record[drive];
		}
		reprap.GetMove()->SetLiveCoordinates(record); // This doesn't transform record
		reprap.GetMove()->SetPositions(record);       // This does
		offSetSet = false;
		return true;
	}

	return false;
}

// Home one or more of the axes.  Which ones are decided by the
// booleans homeX, homeY and homeZ.
// Returns true if completed, false if needs to be called again.
// 'reply' is only written if there is an error.
// 'error' is false on entry, gets changed to true if there is an error.
bool GCodes::DoHome(GCodeBuffer *gb, StringRef& reply, bool& error)
//pre(reply.upb == STRING_LENGTH)
{
	if (!CanStartMacro(gb))
	{
		// If we're interfering with another GCode, wait until it's finished
		return false;
	}

	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		bool rolHome = reprap.GetRoland()->ProcessHome();
		if (rolHome)
		{
			for(size_t axis = 0; axis < AXES; axis++)
			{
				axisIsHomed[axis] = true;
			}
		}
		return rolHome;
	}

	// Homing procedure for delta printers
	if (reprap.GetMove()->IsDeltaMode())
	{
		if (!homing)
		{
			homing = true;
			SetAllAxesNotHomed();
		}

		if (DoFileMacro(gb, HOME_DELTA_G))
		{
			homing = false;
			return true;
		}
		return false;
	}
	// Check which axes are supposed to be homed on other printers
	else if (NoHome())
	{
		homeX = gb->Seen(axisLetters[X_AXIS]);
		homeY = gb->Seen(axisLetters[Y_AXIS]);
		homeZ = gb->Seen(axisLetters[Z_AXIS]);
		if (NoHome())
		{
			homeX = true;
			homeY = true;
			homeZ = true;
		}
	}

	// Homing procedure for cartesian printers
	if (homeX && homeY && homeZ)
	{
		if (!homing)
		{
			homing = true;
			SetAllAxesNotHomed();
		}

		if (DoFileMacro(gb, HOME_ALL_G))
		{
			homing = false;
			homeX = false;
			homeY = false;
			homeZ = false;
			return true;
		}
		return false;
	}

	if (homeX)
	{
		if (!homing)
		{
			homing = true;
			axisIsHomed[X_AXIS] = false;
		}
		if (DoFileMacro(gb, HOME_X_G))
		{
			homing = false;
			homeX = false;
			return NoHome();
		}
		return false;
	}

	if (homeY)
	{
		if (!homing)
		{
			homing = true;
			axisIsHomed[Y_AXIS] = false;
		}
		if (DoFileMacro(gb, HOME_Y_G))
		{
			homing = false;
			homeY = false;
			return NoHome();
		}
		return false;
	}

	if (homeZ)
	{
		if (platform->MustHomeXYBeforeZ() && (!axisIsHomed[X_AXIS] || !axisIsHomed[Y_AXIS]))
		{
			// We can only home Z if X and Y have already been homed
			reply.copy("Must home X and Y before homing Z\n");
			error = true;
			homing = false;
			homeZ = false;
			return true;
		}
		if (!homing)
		{
			homing = true;
			axisIsHomed[Z_AXIS] = false;
		}
		if (DoFileMacro(gb, HOME_Z_G))
		{
			homing = false;
			homeZ = false;
			return NoHome();
		}
		return false;
	}

	// Should never get here

	ClearMove();

	return true;
}

// This lifts Z a bit, moves to the probe XY coordinates (obtained by a call to GetProbeCoordinates() ),
// probes the bed height, and records the Z coordinate probed.  If you want to program any general
// internal canned cycle, this shows how to do it.
bool GCodes::DoSingleZProbeAtPoint(int probePointIndex, float heightAdjust)
{
	reprap.GetMove()->SetIdentityTransform(); // It doesn't matter if these are called repeatedly

	for (size_t drive = 0; drive <= DRIVES; drive++)
	{
		activeDrive[drive] = false;
	}

	switch (cannedCycleMoveCount)
	{
		case 0: // Raise Z. This only does anything on the first move; on all the others Z is already there
			moveToDo[Z_AXIS] = platform->GetZProbeDiveHeight();
			activeDrive[Z_AXIS] = true;
			moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
			activeDrive[DRIVES] = true;
			if (DoCannedCycleMove(0))
			{
				cannedCycleMoveCount++;
			}
			return false;

		case 1:	// Move to the correct XY coordinates
			GetProbeCoordinates(probePointIndex, moveToDo[X_AXIS], moveToDo[Y_AXIS], moveToDo[Z_AXIS]);
			activeDrive[X_AXIS] = true;
			activeDrive[Y_AXIS] = true;
			// NB - we don't use the Z value
			moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
			activeDrive[DRIVES] = true;
			if (DoCannedCycleMove(0))
			{
				cannedCycleMoveCount++;
			}
			return false;

		case 2:	// Probe the bed
			{
				const float height = (axisIsHomed[Z_AXIS])
					? 2 * platform->GetZProbeDiveHeight()			// Z axis has been homed, so no point in going very far
					: 1.1 * platform->AxisTotalLength(Z_AXIS);		// Z axis not homed yet, so treat this as a homing move

				switch (DoZProbe(height))
				{
					case 0:
						// Z probe is already triggered at the start of the move, so abandon the probe and record an error
						platform->Message(GENERIC_MESSAGE, "Z probe warning: probe already triggered at start of probing move\n");
						cannedCycleMoveCount++;
						reprap.GetMove()->SetZBedProbePoint(probePointIndex, platform->GetZProbeDiveHeight(), true, true);
						break;

					case 1:
						if (axisIsHomed[Z_AXIS])
						{
							lastProbedZ = moveBuffer[Z_AXIS] - (platform->ZProbeStopHeight() + heightAdjust);
						}
						else
						{
							// The Z axis has not yet been homed, so treat this probe as a homing move.
							moveBuffer[Z_AXIS] = platform->ZProbeStopHeight() + heightAdjust;
							SetPositions(moveBuffer);
							axisIsHomed[Z_AXIS] = true;
							lastProbedZ = 0.0;
						}
						reprap.GetMove()->SetZBedProbePoint(probePointIndex, lastProbedZ, true, false);
						cannedCycleMoveCount++;
						break;

					default:
						break;
				}
			}
			return false;

		case 3:	// Raise the head
			moveToDo[Z_AXIS] = platform->GetZProbeDiveHeight();
			activeDrive[Z_AXIS] = true;
			moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
			activeDrive[DRIVES] = true;
			if (DoCannedCycleMove(0))
			{
				cannedCycleMoveCount = 0;
				return true;
			}
			return false;

		default:
			cannedCycleMoveCount = 0;
			return true;
	}
}

// This simply moves down till the Z probe/switch is triggered. Call it repeatedly until it returns true.
// Called when we do a G30 with no P parameter.
bool GCodes::DoSingleZProbe(bool reportOnly, float heightAdjust)
{
	switch (DoZProbe(1.1 * platform->AxisTotalLength(Z_AXIS)))
	{
		case 0:		// failed
			return true;

		case 1:		// success
			if (!reportOnly)
			{
				moveBuffer[Z_AXIS] = platform->ZProbeStopHeight() + heightAdjust;
				SetPositions(moveBuffer);
				axisIsHomed[Z_AXIS] = true;
				lastProbedZ = 0.0;
			}
			return true;

		default:	// not finished yet
			return false;
	}
}

// Do a Z probe cycle up to the maximum specified distance.
// Returns -1 if not complete yet
// Returns 0 if failed
// Returns 1 if success, with lastProbedZ set to the height we stopped at and the current position in moveBuffer
int GCodes::DoZProbe(float distance)
{
	if (platform->GetZProbeType() == 5)
	{
		const ZProbeParameters& params = platform->GetZProbeParameters();
		return reprap.GetMove()->DoDeltaProbe(params.param1, params.param2, params.probeSpeed, distance);
	}
	else
	{
		if (!cannedCycleMoveQueued && reprap.GetPlatform()->GetZProbeResult() == EndStopHit::lowHit)
		{
			return 0;
		}

		// Do a normal canned cycle Z movement with Z probe enabled
		for (size_t drive = 0; drive <= DRIVES; drive++)
		{
			activeDrive[drive] = false;
		}

		moveToDo[Z_AXIS] = -distance;
		activeDrive[Z_AXIS] = true;
		moveToDo[DRIVES] = platform->GetZProbeParameters().probeSpeed;
		activeDrive[DRIVES] = true;

		if (DoCannedCycleMove(ZProbeActive))
		{
			return 1;
		}
		return -1;
	}
}

// This sets wherever we are as the probe point P (probePointIndex)
// then probes the bed, or gets all its parameters from the arguments.
// If X or Y are specified, use those; otherwise use the machine's
// coordinates.  If no Z is specified use the machine's coordinates.  If it
// is specified and is greater than SILLY_Z_VALUE (i.e. greater than -9999.0)
// then that value is used.  If it's less than SILLY_Z_VALUE the bed is
// probed and that value is used.
bool GCodes::SetSingleZProbeAtAPosition(GCodeBuffer *gb, StringRef& reply)
{
	float heightAdjust = 0.0;
	if (gb->Seen('H'))
	{
		heightAdjust = gb->GetFValue();
	}

	if (!gb->Seen('P'))
	{
		bool reportOnly = false;
		if (gb->Seen('S') && gb->GetIValue() < 0)
		{
			reportOnly = true;
		}
		return DoSingleZProbe(reportOnly, heightAdjust);
	}

	int probePointIndex = gb->GetIValue();
	if (probePointIndex < 0 || (unsigned int)probePointIndex >= MAX_PROBE_POINTS)
	{
		reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Z probe point index out of range.\n");
		return true;
	}

	float x = (gb->Seen(axisLetters[X_AXIS])) ? gb->GetFValue() : moveBuffer[X_AXIS];
	float y = (gb->Seen(axisLetters[Y_AXIS])) ? gb->GetFValue() : moveBuffer[Y_AXIS];
	float z = (gb->Seen(axisLetters[Z_AXIS])) ? gb->GetFValue() : moveBuffer[Z_AXIS];

	reprap.GetMove()->SetXBedProbePoint(probePointIndex, x);
	reprap.GetMove()->SetYBedProbePoint(probePointIndex, y);

	if (z > SILLY_Z_VALUE)
	{
		reprap.GetMove()->SetZBedProbePoint(probePointIndex, z, false, false);
		if (gb->Seen('S'))
		{
			zProbesSet = true;
			reprap.GetMove()->FinishedBedProbing(gb->GetIValue(), reply);
		}
		return true;
	}
	else
	{
		if (DoSingleZProbeAtPoint(probePointIndex, heightAdjust))
		{
			if (gb->Seen('S'))
			{
				zProbesSet = true;
				int sParam = gb->GetIValue();
				if (sParam == 1)
				{
					// G30 with a silly Z value and S=1 is equivalent to G30 with no parameters in that it sets the current Z height
					// This is useful because it adjusts the XY position to account for the probe offset.
					moveBuffer[Z_AXIS] += lastProbedZ;
					SetPositions(moveBuffer);
					lastProbedZ = 0.0;
				}
				else
				{
					reprap.GetMove()->FinishedBedProbing(sParam, reply);
				}
			}
			return true;
		}
	}

	return false;
}

// This probes multiple points on the bed (three in a
// triangle or four in the corners), then sets the bed transformation to compensate
// for the bed not quite being the plane Z = 0.
bool GCodes::SetBedEquationWithProbe(const GCodeBuffer *gb, StringRef& reply)
{
	if (!CanStartMacro(gb))
	{
		// If we're interfering with another GCode, wait until it's finished
		return false;
	}

	// In order to stay compatible with older firmware versions,
	// only execute bed.g if it is actually present in the /sys directory.
	const char *absoluteBedGPath = platform->GetMassStorage()->CombineName(SYS_DIR, BED_EQUATION_G);
	if (platform->GetMassStorage()->FileExists(absoluteBedGPath))
	{
		if (DoFileMacro(gb, absoluteBedGPath))
		{
			settingBedEquationWithProbe = false;
			return true;
		}

		settingBedEquationWithProbe = true;
		return false;
	}

	if (reprap.GetMove()->NumberOfXYProbePoints() < 3)
	{
		reply.copy("Bed probing: there needs to be 3 or more points set.\n");
		return true;
	}

	// When calling G32, ensure bed compensation parameters are initially reset
	if (!settingBedEquationWithProbe)
	{
		reprap.GetMove()->SetIdentityTransform();
		settingBedEquationWithProbe = true;
	}

	if (DoSingleZProbeAtPoint(probeCount, 0.0))
	{
		probeCount++;
	}

	if (probeCount >= reprap.GetMove()->NumberOfXYProbePoints())
	{
		probeCount = 0;
		zProbesSet = true;
		reprap.GetMove()->FinishedBedProbing(0, reply);
		settingBedEquationWithProbe = false;
		return true;
	}
	return false;
}

// This returns the (X, Y) points to probe the bed at probe point count.  When probing,
// it returns false.  If called after probing has ended it returns true, and the Z coordinate
// probed is also returned.
bool GCodes::GetProbeCoordinates(int count, float& x, float& y, float& z) const
{
	const ZProbeParameters& rp = platform->GetZProbeParameters();
	x = reprap.GetMove()->XBedProbePoint(count) - rp.xOffset;
	y = reprap.GetMove()->YBedProbePoint(count) - rp.yOffset;
	z = reprap.GetMove()->ZBedProbePoint(count);
	return zProbesSet;
}

bool GCodes::SetPrintZProbe(GCodeBuffer* gb, StringRef& reply)
{
	ZProbeParameters params = platform->GetZProbeParameters();
	bool seen = false;
	if (gb->Seen(axisLetters[X_AXIS]))
	{
		params.xOffset = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen(axisLetters[Y_AXIS]))
	{
		params.yOffset = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen(axisLetters[Z_AXIS]))
	{
		params.height = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen('P'))
	{
		params.adcValue = gb->GetIValue();
		seen = true;
	}
	if (gb->Seen('C'))
	{
		params.temperatureCoefficient = gb->GetFValue();
		seen = true;
		if (gb->Seen('S'))
		{
			params.calibTemperature = gb->GetFValue();
		}
		else if (reprap.GetHeat()->GetBedHeater() < 0)
		{
			reply.copy("Error: Could not set calibration temperature, because no heated bed is configured!\n");
		}
		else
		{
			// Use the current bed temperature as the calibration temperature if no value was provided
			params.calibTemperature = platform->GetTemperature(reprap.GetHeat()->GetBedHeater());
		}
	}

	if (seen)
	{
		platform->SetZProbeParameters(params);
	}
	else
	{
		int v0 = platform->ZProbe();
		int v1, v2;
		switch (platform->GetZProbeSecondaryValues(v1, v2))
		{
			case 1:
				reply.printf("%d (%d)\n", v0, v1);
				break;
			case 2:
				reply.printf("%d (%d, %d)\n", v0, v1, v2);
				break;
			default:
				reply.printf("%d\n", v0);
				break;
		}
	}
	return true;
}

// Return the current coordinates as a printable string.  Coordinates
// are updated at the end of each movement, so this won't tell you
// where you are mid-movement.

// Fixed to deal with multiple extruders
void GCodes::GetCurrentCoordinates(StringRef &s) const
{
	float liveCoordinates[DRIVES + 1];
	reprap.GetMove()->LiveCoordinates(liveCoordinates);
	const Tool *currentTool = reprap.GetCurrentTool();
	if (currentTool != nullptr)
	{
		const float *offset = currentTool->GetOffset();
		for (size_t i = 0; i < AXES; ++i)
		{
			liveCoordinates[i] += offset[i];
		}
	}

	s.printf("X:%.2f Y:%.2f Z:%.2f ", liveCoordinates[X_AXIS], liveCoordinates[Y_AXIS], liveCoordinates[Z_AXIS]);
	for(size_t i = AXES; i< DRIVES; i++)
	{
		s.catf("E%u:%.1f ", i-AXES, liveCoordinates[i]);
	}

	// Print the stepper motor positions as Marlin does, as an aid to debugging
	s.cat(" Count");
	for (size_t i = 0; i < DRIVES; ++i)
	{
		s.catf(" %d", reprap.GetMove()->GetEndPoint(i));
	}
	s.cat("\n");
}

float GCodes::FractionOfFilePrinted() const
{
	if (fractionOfFilePrinted >= 0.0)
	{
		return fractionOfFilePrinted;
	}

	if (DoingFileMacro() && !fileToPrint.IsLive())
	{
		return -1.0;
	}

	if (IsPaused() && fileToPrint.IsLive())
	{
		return fileToPrint.FractionRead();
	}

	return fileBeingPrinted.FractionRead();
}

bool GCodes::OpenFileToWrite(const char* directory, const char* fileName, GCodeBuffer *gb)
{
	fileBeingWritten = platform->GetFileStore(directory, fileName, true);
	eofStringCounter = 0;
	if (fileBeingWritten == nullptr)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't open GCode file \"%s\" for writing.\n", fileName);
		return false;
	}
	else
	{
		gb->SetWritingFileDirectory(directory);
		return true;
	}
}

void GCodes::WriteHTMLToFile(char b, GCodeBuffer *gb)
{
	if (fileBeingWritten == nullptr)
	{
		platform->Message(GENERIC_MESSAGE, "Attempt to write to a null file.\n");
		return;
	}

	if (eofStringCounter != 0 && b != EOF_STRING[eofStringCounter])
	{
		for(size_t i = 0; i < eofStringCounter; ++i)
		{
			fileBeingWritten->Write(EOF_STRING[i]);
		}
		eofStringCounter = 0;
	}

	if (b == EOF_STRING[eofStringCounter])
	{
		eofStringCounter++;
		if (eofStringCounter >= ARRAY_UPB(EOF_STRING))
		{
			fileBeingWritten->Close();
			fileBeingWritten = nullptr;
			gb->SetWritingFileDirectory(nullptr);
			const char* r = (platform->Emulating() == marlin) ? "Done saving file.\n" : "";
			HandleReply(gb, false, r);
			return;
		}
	}
	else
	{
		fileBeingWritten->Write(b);
	}
}

void GCodes::WriteGCodeToFile(GCodeBuffer *gb)
{
	if (fileBeingWritten == nullptr)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write to a null file.\n");
		return;
	}

	// End of file?

	if (gb->Seen('M'))
	{
		if (gb->GetIValue() == 29)
		{
			fileBeingWritten->Close();
			fileBeingWritten = nullptr;
			gb->SetWritingFileDirectory(nullptr);
			const char* r = (platform->Emulating() == marlin) ? "Done saving file.\n" : "";
			HandleReply(gb, false, r);
			return;
		}
	}

	// Resend request?

	if (gb->Seen('M') && gb->GetIValue() == 998)
	{
		if (gb->Seen('P'))
		{
			char temp[16];
			snprintf(temp, ARRAY_SIZE(temp), "%d\n", gb->GetIValue());
			HandleReply(gb, false, temp);
			return;
		}
	}

	fileBeingWritten->Write(gb->Buffer());
	fileBeingWritten->Write('\n');
	HandleReply(gb, false, "");
}

// Set up a file to print, but don't print it yet.
void GCodes::QueueFileToPrint(const char* fileName)
{
	FileStore *f = platform->GetFileStore(platform->GetGCodeDir(), fileName, false);
	if (f != nullptr)
	{
		// Cancel current print if there is any
		if (reprap.GetPrintMonitor()->IsPrinting())
		{
			CancelPrint();
		}

		fileGCode->SetToolNumberAdjust(0);

		for (size_t extruder = AXES; extruder < DRIVES; extruder++)
		{
			lastExtruderPosition[extruder - AXES] = 0.0;
		}
		reprap.GetMove()->ResetRawExtruderTotals();

		fileToPrint.Set(f);
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: GCode file \"%s\" not found\n", fileName);
	}
}

void GCodes::DeleteFile(const char* fileName)
{
	if (!platform->GetMassStorage()->Delete(platform->GetGCodeDir(), fileName))
	{
		platform->MessageF(GENERIC_MESSAGE, "Could not delete file \"%s\"\n", fileName);
	}
}

// Function to handle dwell delays.  Return true for dwell finished, false otherwise.
bool GCodes::DoDwell(GCodeBuffer *gb)
{
	if (!gb->Seen('P'))
		return true;  // No time given - throw it away

	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		return reprap.GetRoland()->ProcessDwell(gb->GetLValue());
	}

	// Wait for all the queued moves to stop
	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
	{
		return false;
	}

	float dwell = 0.001 * (float) gb->GetLValue(); // P values are in milliseconds; we need seconds

	if (simulating)
	{
		simulationTime += dwell;
		return true;
	}
	else
	{
		return DoDwellTime(dwell);
	}
}

bool GCodes::DoDwellTime(float dwell)
{
	// Are we already in the dwell?
	if (dwellWaiting)
	{
		if (platform->Time() - dwellTime >= 0.0)
		{
			dwellWaiting = false;
			reprap.GetMove()->ResumeMoving();
			return true;
		}
		return false;
	}

	// New dwell - set it up
	reprap.GetMove()->AllMovesAreFinished();
	dwellWaiting = true;
	dwellTime = platform->Time() + dwell;
	return false;
}

// Set offset, working and standby temperatures for a tool. I.e. handle a G10.
void GCodes::SetOrReportOffsets(StringRef& reply, GCodeBuffer *gb)
{
	if (gb->Seen('P'))
	{
		int toolNumber = gb->GetIValue();
		toolNumber += gb->GetToolNumberAdjust();
		Tool* tool = reprap.GetTool(toolNumber);
		if (tool == nullptr)
		{
			reply.printf("Attempt to set/report offsets and temperatures for non-existent tool: %d\n", toolNumber);
			return;
		}

		// Deal with setting offsets
		float offset[AXES];
		for(size_t i = 0; i < AXES; ++i)
		{
			offset[i] = tool->GetOffset()[i];
		}

		bool settingOffset = false;
		for(size_t axis = 0; axis < AXES; axis++)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				offset[axis] = gb->GetFValue();
				settingOffset = true;
			}
		}

		if (settingOffset)
		{
			tool->SetOffset(offset);
		}

		// Deal with setting temperatures
		bool settingTemps = false;
		size_t hCount = tool->HeaterCount();
		float standby[HEATERS];
		float active[HEATERS];
		if (hCount > 0)
		{
			tool->GetVariables(standby, active);
			if (gb->Seen('R'))
			{
				gb->GetFloatArray(standby, hCount);
				settingTemps = true;
			}
			if (gb->Seen('S'))
			{
				gb->GetFloatArray(active, hCount);
				settingTemps = true;
			}

			if (settingTemps && !simulating)
			{
				tool->SetVariables(standby, active);
			}
		}

		if (!settingOffset && !settingTemps)
		{
			// Print offsets and temperatures
			reply.printf("Tool %d offsets: X%.1f Y%.1f Z%.1f", toolNumber, offset[X_AXIS], offset[Y_AXIS], offset[Z_AXIS]);
			if (hCount != 0)
			{
				reply.cat(", active/standby temperature(s):");
				for(size_t heater = 0; heater < hCount; heater++)
				{
					reply.catf(" %.1f/%.1f", active[heater], standby[heater]);
				}
			}
			reply.cat("\n");
		}
	}
}

void GCodes::ManageTool(GCodeBuffer *gb, StringRef& reply)
{
	if(!gb->Seen('P'))
	{
		// DC temporary code to allow tool numbers to be adjusted so that we don't need to edit multi-media files generated by slic3r
		if (gb->Seen('S'))
		{
			int adjust = gb->GetIValue();
			gb->SetToolNumberAdjust(adjust);
		}
		return;
	}

	// Check tool number
	bool seen = false;
	int toolNumber = gb->GetLValue();
	if (toolNumber < 0)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Tool number must be positive!\n");
		return;
	}

	// Check drives
	long drives[DRIVES - AXES];		// There can never be more than we have...
	size_t dCount = DRIVES - AXES;	// Sets the limit and returns the count
	if(gb->Seen('D'))
	{
		gb->GetLongArray(drives, dCount);
		seen = true;
	}
	else
	{
		dCount = 0;
	}

	// Check heaters
	long heaters[HEATERS];
	size_t hCount = HEATERS;
	if(gb->Seen('H'))
	{
		gb->GetLongArray(heaters, hCount);
		seen = true;
	}
	else
	{
		hCount = 0;
	}

	// Add or delete tool
	if (seen)
	{
		// M563 P# D-1 H-1 removes an existing tool
		if (dCount == 1 && hCount == 1 && drives[0] == -1 && heaters[0] == -1)
		{
			Tool *tool = reprap.GetTool(toolNumber);
			reprap.DeleteTool(tool);
		}
		else if (reprap.GetTool(toolNumber) != nullptr)
		{
			reprap.GetPlatform()->MessageF(GENERIC_MESSAGE, "Error: Tool number %d already in use!\n", toolNumber);
		}
		else
		{
			Tool* tool = new Tool(toolNumber, drives, dCount, heaters, hCount);
			reprap.AddTool(tool);
		}
	}
	else
	{
		reprap.PrintTool(toolNumber, reply);
	}
}

// Does what it says.
void GCodes::DisableDrives()
{
	for(size_t drive = 0; drive < DRIVES; drive++)
	{
		platform->DisableDrive(drive);
	}

	axisIsHomed[X_AXIS] = false;
	axisIsHomed[Y_AXIS] = false;
	axisIsHomed[Z_AXIS] = false;
}

// Does what it says.
void GCodes::SetEthernetAddress(GCodeBuffer *gb, int mCode)
{
	byte eth[4];
	const char* ipString = gb->GetString();
	uint8_t sp = 0;
	uint8_t spp = 0;
	uint8_t ipp = 0;
	while (ipString[sp])
	{
		if (ipString[sp] == '.')
		{
			eth[ipp] = atoi(&ipString[spp]);
			ipp++;
			if (ipp > 3)
			{
				platform->MessageF(GENERIC_MESSAGE, "Error: Dud IP address: %s\n", gb->Buffer());
				return;
			}
			sp++;
			spp = sp;
		}
		else
		{
			sp++;
		}
	}
	eth[ipp] = atoi(&ipString[spp]);
	if (ipp == 3)
	{
		switch (mCode)
		{
			case 552:
				platform->SetIPAddress(eth);
				break;
			case 553:
				platform->SetNetMask(eth);
				break;
			case 554:
				platform->SetGateWay(eth);
				break;

			default:
				platform->Message(GENERIC_MESSAGE, "Error: Setting ether parameter - dud code.\n");
		}
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Dud IP address: %s\n", gb->Buffer());
	}
}

void GCodes::SetMACAddress(GCodeBuffer *gb)
{
	uint8_t mac[6];
	const char* ipString = gb->GetString();
	uint8_t sp = 0;
	uint8_t spp = 0;
	uint8_t ipp = 0;
	while(ipString[sp])
	{
		if(ipString[sp] == ':')
		{
			mac[ipp] = strtoul(&ipString[spp], nullptr, 16);
			ipp++;
			if(ipp > 5)
			{
				platform->MessageF(GENERIC_MESSAGE, "Error: Dud MAC address: %s\n", gb->Buffer());
				return;
			}
			sp++;
			spp = sp;
		}
		else
		{
			sp++;
		}
	}
	mac[ipp] = strtoul(&ipString[spp], nullptr, 16);
	if(ipp == 5)
	{
		platform->SetMACAddress(mac);
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Dud MAC address: %s\n", gb->Buffer());
	}
}

void GCodes::HandleReply(GCodeBuffer *gb, bool error, const char *reply)
{
	// If we're executing a queued code, make sure gb points to the right source
	if (gb == queueGCode)
	{
		gb = queueGCodeSource;
	}

	// Don't report "ok" responses if a (macro) file is being processed
	if ((gb == fileMacroGCode || gb == fileGCode) && reply[0] == 0)
	{
		return;
	}

	// Make sure macro responses are sent to the right destination
	if (gb == fileMacroGCode)
	{
		gb = const_cast<GCodeBuffer *>(macroSourceGCodeBuffer);
	}

	// Second UART device, e.g. dc42's PanelDue. Do NOT use emulation for this one!
	if (gb == auxGCode)
	{
		// Discard this response if either no aux device is attached or if the response is empty
		if (!reply[0] || !HaveAux())
		{
			return;
		}

		// Regular text-based responses for AUX are always stored and processed by M105/M408
		if (auxGCodeReply == nullptr && !OutputBuffer::Allocate(auxGCodeReply))
		{
			// No more space to buffer this response. Should never happen
			return;
		}
		auxSeq++;
		auxGCodeReply->cat(reply);

		return;
	}

	// Where do we need to send this reply?
	MessageType type = GENERIC_MESSAGE;
	if (gb == httpGCode)
	{
		type = HTTP_MESSAGE;
	}
	else if (gb == telnetGCode)
	{
		type = TELNET_MESSAGE;
	}
	else if (gb == serialGCode)
	{
		type = HOST_MESSAGE;
	}

	// What kind of emulation should be used?
	const Compatibility c = (gb == serialGCode || gb == telnetGCode) ? platform->Emulating() : me;
	const char* response = (gb->Seen('M') && gb->GetIValue() == 998) ? "rs " : "ok";
	const char* emulationType = nullptr;

	switch (c)
	{
		case me:
		case reprapFirmware:
			if (error)
			{
				platform->Message(type, "Error: ");
			}
			platform->Message(type, reply);
			return;

		case marlin:
			if (gb->Seen('M') && gb->GetIValue() == 20)
			{
				platform->Message(type, "Begin file list\n");
				platform->Message(type, reply);
				platform->Message(type, "End file list\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
				return;
			}

			if (gb->Seen('M') && gb->GetIValue() == 28)
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
				platform->Message(type, reply);
				return;
			}

			if ((gb->Seen('M') && gb->GetIValue() == 105) || (gb->Seen('M') && gb->GetIValue() == 998))
			{
				platform->Message(type, response);
				platform->Message(type, " ");
				platform->Message(type, reply);
				return;
			}

			if (reply[0] && !DoingFileMacro())
			{
				platform->Message(type, reply);
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			else if (reply[0])
			{
				platform->Message(type, reply);
			}
			else
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			return;

		case teacup:
			emulationType = "teacup";
			break;
		case sprinter:
			emulationType = "sprinter";
			break;
		case repetier:
			emulationType = "repetier";
			break;
		default:
			emulationType = "unknown";
	}

	if (emulationType != nullptr)
	{
		platform->MessageF(type, "Emulation of %s is not yet supported.\n", emulationType);	// don't send this one to the web as well, it concerns only the USB interface
	}
}

void GCodes::HandleReply(GCodeBuffer *gb, bool error, OutputBuffer *reply)
{
	// Although unlikely, it's possible that we get a nullptr reply. Don't proceed if this is the case
	if (reply == nullptr)
	{
		return;
	}

	// If we're executing a queued code or a macro G-code, make sure gb points to the right source
	if (gb == queueGCode)
	{
		gb = queueGCodeSource;
	}
	else if (gb == fileMacroGCode)
	{
		gb = const_cast<GCodeBuffer *>(macroSourceGCodeBuffer);
	}

	// Second UART device, e.g. dc42's PanelDue. Do NOT use emulation for this one!
	if (gb == auxGCode)
	{
		// Discard this response if either no aux device is attached or if the response is empty
		if (reply->Length() == 0 || !HaveAux())
		{
			OutputBuffer::ReleaseAll(reply);
			return;
		}

		// JSON responses are always sent directly to the AUX device
		if ((*reply)[0] == '{')
		{
			platform->Message(AUX_MESSAGE, reply);
			return;
		}

		// Other responses are stored for M105/M408
		auxSeq++;
		if (auxGCodeReply == nullptr)
		{
			auxGCodeReply = reply;
		}
		else
		{
			auxGCodeReply->Append(reply);
		}
		return;
	}

	// Where do we need to send this reply?
	MessageType type = GENERIC_MESSAGE;
	if (gb == httpGCode)
	{
		type = HTTP_MESSAGE;
	}
	else if (gb == telnetGCode)
	{
		type = TELNET_MESSAGE;
	}
	else if (gb == serialGCode)
	{
		type = HOST_MESSAGE;
	}

	// What kind of emulation should be used?
	const Compatibility c = (gb == serialGCode || gb == telnetGCode) ? platform->Emulating() : me;
	const char* response = (gb->Seen('M') && gb->GetIValue() == 998) ? "rs " : "ok";
	const char* emulationType = nullptr;

	switch (c)
	{
		case me:
		case reprapFirmware:
			if (error)
			{
				platform->Message(type, "Error: ");
				platform->Message(type, reply);
			}
			else
			{
				platform->Message(type, reply);
			}
			return;

		case marlin:
			if (gb->Seen('M') && gb->GetIValue() == 20)
			{
				platform->Message(type, "Begin file list\n");
				platform->Message(type, reply);
				platform->Message(type, "End file list\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
				return;
			}

			if (gb->Seen('M') && gb->GetIValue() == 28)
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
				platform->Message(type, reply);
				return;
			}

			if ((gb->Seen('M') && gb->GetIValue() == 105) || (gb->Seen('M') && gb->GetIValue() == 998))
			{
				platform->Message(type, response);
				platform->Message(type, " ");
				platform->Message(type, reply);
				return;
			}

			if (reply->Length() != 0 && !DoingFileMacro())
			{
				platform->Message(type, reply);
				platform->Message(type, "\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			else if (reply->Length() != 0)
			{
				platform->Message(type, reply);
			}
			else
			{
				OutputBuffer::ReleaseAll(reply);
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			return;

		case teacup:
			emulationType = "teacup";
			break;
		case sprinter:
			emulationType = "sprinter";
			break;
		case repetier:
			emulationType = "repetier";
			break;
		default:
			emulationType = "unknown";
	}

	// If we haven't sent the reply yet, free it here
	OutputBuffer::ReleaseAll(reply);

	if (emulationType != nullptr)
	{
		// Don't send this one to the web as well, it concerns only the USB and Telnet interfaces
		platform->MessageF(type, "Emulation of %s is not yet supported.\n", emulationType);
	}
}

// Set PID parameters (M301 or M304 command). 'heater' is the default heater number to use.
void GCodes::SetPidParameters(GCodeBuffer *gb, int heater, StringRef& reply)
{
	if (gb->Seen('H'))
	{
		heater = gb->GetIValue();
	}

	if (heater >= 0 && heater < HEATERS)
	{
		PidParameters pp = platform->GetPidParameters(heater);
		bool seen = false;
		if (gb->Seen('P'))
		{
			pp.kP = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('I'))
		{
			pp.kI = gb->GetFValue() / platform->HeatSampleTime();
			seen = true;
		}
		if (gb->Seen('D'))
		{
			pp.kD = gb->GetFValue() * platform->HeatSampleTime();
			seen = true;
		}
		if (gb->Seen('T'))
		{
			pp.kT = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('S'))
		{
			pp.kS = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('W'))
		{
			pp.pidMax = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('B'))
		{
			pp.fullBand = gb->GetFValue();
			seen = true;
		}

		if (seen)
		{
			platform->SetPidParameters(heater, pp);
		}
		else
		{
			reply.printf("Heater %d P:%.2f I:%.3f D:%.2f T:%.2f S:%.2f W:%.1f B:%.1f\n",
					    heater, pp.kP, pp.kI * platform->HeatSampleTime(), pp.kD / platform->HeatSampleTime(), pp.kT, pp.kS, pp.pidMax, pp.fullBand);
		}
	}
}

void GCodes::SetHeaterParameters(GCodeBuffer *gb, StringRef& reply)
{
	if (gb->Seen('P'))
	{
		int heater = gb->GetIValue();
		if (heater >= 0 && heater < HEATERS)
		{
			PidParameters pp = platform->GetPidParameters(heater);
			bool seen = false;

			// We must set the 25C resistance and beta together in order to calculate Rinf. Check for these first.
			float r25, beta;
			if (gb->Seen('T'))
			{
				r25 = gb->GetFValue();
				seen = true;
			}
			else
			{
				r25 = pp.GetThermistorR25();
			}
			if (gb->Seen('B'))
			{
				beta = gb->GetFValue();
				seen = true;
			}
			else
			{
				beta = pp.GetBeta();
			}

			if (seen)	// if see R25 or Beta or both
			{
				pp.SetThermistorR25AndBeta(r25, beta);					// recalculate Rinf
			}

			// Now do the other parameters
			if (gb->Seen('R'))
			{
				pp.thermistorSeriesR = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('L'))
			{
				pp.adcLowOffset = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('H'))
			{
				pp.adcHighOffset = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('X'))
			{
				int thermistor = gb->GetIValue();
				if (   (0 <= thermistor && thermistor < HEATERS)
						|| ((int)MAX31855_START_CHANNEL <= thermistor && thermistor < (int)(MAX31855_START_CHANNEL + MAX31855_DEVICES))
				   )
				{
					platform->SetThermistorNumber(heater, thermistor);
				}
				else
				{
					platform->MessageF(GENERIC_MESSAGE, "Error: Thermistor number %d is out of range\n", thermistor);
				}
				seen = true;
			}

			if (seen)
			{
				platform->SetPidParameters(heater, pp);
			}
			else
			{
				reply.printf("T:%.1f B:%.1f R:%.1f L:%.1f H:%.1f X:%d\n",
						r25, beta, pp.thermistorSeriesR, pp.adcLowOffset, pp.adcHighOffset, platform->GetThermistorNumber(heater));
			}
		}
		else
		{
			platform->MessageF(GENERIC_MESSAGE, "Error: Heater number %d is out of range\n", heater);
		}
	}
}

void GCodes::SetToolHeaters(Tool *tool, float temperature)
{
	if (tool == nullptr)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Setting temperature: no tool selected.\n");
		return;
	}

	float standby[HEATERS];
	float active[HEATERS];
	tool->GetVariables(standby, active);
	for(size_t h = 0; h < tool->HeaterCount(); h++)
	{
		active[h] = temperature;
	}
	tool->SetVariables(standby, active);
}

// Here we must check for codes that shall be executed as soon as all currently queued moves have finished.
bool GCodes::CanQueueCode(GCodeBuffer *gb) const
{
	// Check for G-Codes
	if (gb->Seen('G'))
	{
		const int code = gb->GetIValue();

		// Set active/standby temperatures
		if (code == 10 && (gb->Seen('R') || gb->Seen('S')))
			return true;
	}
	// Check for M-Codes
	else if (gb->Seen('M'))
	{
		const int code = gb->GetIValue();

		// Fan control
		if (code == 106 || code == 107)
			return true;

		// Set temperatures and return immediately
		if (code == 104 || code == 140 || code == 141 || code == 144)
			return true;

		// Display Message (LCD), Beep, RGB colour, Set servo position
		if (code == 117 || code == 300 || code == 280 || code == 420)
			return true;

		// Valve control
		if (code == 126 || code == 127)
			return true;

		// Set networking parameters, Emulation, Compensation, Z-Probe changes
		// File Uploads, Tool management
		if (code == 540 || (code >= 550 && code <= 563))
			return true;

		// Move, heater and auxiliary PWM control
		if (code >= 566 && code <= 573)
			return true;

		// Motor currents
		if (code == 906)
			return true;
	}

	return false;
}

// If the code to act on is completed, this returns true,
// otherwise false.  It is called repeatedly for a given
// code until it returns true for that code.
bool GCodes::ActOnCode(GCodeBuffer *gb, bool executeImmediately)
{
	// Discard empty buffers right away
	if (gb->IsEmpty())
	{
		return true;
	}

	// Check if we can execute this code immediately
	if (executeImmediately || totalMoves == movesCompleted || !CanQueueCode(gb))
	{
		// M-code parameters might contain letters T and G, e.g. in filenames.
		// dc42 assumes that G-and T-code parameters never contain the letter M.
		// Therefore we must check for an M-code first.
		if (gb->Seen('M'))
		{
			return HandleMcode(gb);
		}
		// dc42 doesn't think a G-code parameter ever contains letter T, or a T-code ever contains letter G.
		// So it doesn't matter in which order we look for them.
		if (gb->Seen('G'))
		{
			return HandleGcode(gb);
		}
		if (gb->Seen('T'))
		{
			return HandleTcode(gb);
		}

		// An invalid buffer gets discarded
		HandleReply(gb, false, "");
		return true;
	}

	// Try to queue this code. If we cannot store it, attempt to run the first queued code and wait for it to complete
	if (releasedQueueItems == nullptr)
	{
		if (queueGCode->Active())
		{
			// Cannot do anything. The last queued G-Code has not finished yet
			return false;
		}

		// Run the first item
		queueGCodeSource = internalCodeQueue->GetSource();
		if (!queueGCode->Put(internalCodeQueue->GetCode(), internalCodeQueue->GetCodeLength()))
		{
			queueGCode->Put('\n');
		}

		// Remove it from the chain
		CodeQueueItem *temp = internalCodeQueue;
		internalCodeQueue = internalCodeQueue->Next();
		temp->SetNext(releasedQueueItems);
		releasedQueueItems = temp;
	}

	// Set up a new queue item
	CodeQueueItem *newItem = releasedQueueItems;
	releasedQueueItems = releasedQueueItems->Next();
	newItem->Init(gb, totalMoves);

	// Append it so it's executed once all pending moves have finished
	if (internalCodeQueue == nullptr)
	{
		internalCodeQueue = newItem;
	}
	else
	{
		CodeQueueItem *lastItem = internalCodeQueue;
		while (lastItem->Next() != nullptr)
		{
			lastItem = lastItem->Next();
		}
		lastItem->SetNext(newItem);
	}
	return true;
}

bool GCodes::HandleGcode(GCodeBuffer* gb)
{
	bool result = true;
	bool error = false;
	char replyBuffer[LONG_STRING_LENGTH];
	StringRef reply(replyBuffer, ARRAY_SIZE(replyBuffer));
	reply.Clear();

	int code = gb->GetIValue();
	if (simulating && code != 0 && code != 1 && code != 4 && code != 10 && code != 20 && code != 21 && code != 90
			&& code != 91 && code != 92)
	{
		HandleReply(gb, false, "");
		return true;                    // we only simulate some gcodes
	}

	switch (code)
	{
		case 0: // There are no rapid moves...
		case 1: // Ordinary move
			if (waitingForMoveGCodeBuffer != nullptr)
			{
				if (waitingForMoveGCodeBuffer == gb && AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					// Homing move is complete. Reset the GCodeBuffer reference and move on
					waitingForMoveGCodeBuffer = nullptr;
					result = true;
				}
				else
				{
					// Cannot perform any moves while a homing move is being processed. Wait for it to finish first
					result = false;
				}
			}
			else
			{
				if (gb == fileGCode && !IsRunning())
				{
					// File print has been paused. Discard this move, it will be called again when the print is resumed
					return true;
				}

				// Set up a new move
				int res = SetUpMove(gb, reply);
				result = (res == 1);
				if (res == 2)
				{
					waitingForMoveGCodeBuffer = gb;
				}
			}
			break;

		case 4: // Dwell
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			result = DoDwell(gb);
			break;

		case 10: // Set/report offsets and temperatures
			SetOrReportOffsets(reply, gb);
			break;

		case 20: // Inches (which century are we living in, here?)
			distanceScale = INCH_TO_MM;
			break;

		case 21: // mm
			distanceScale = 1.0;
			break;

		case 28: // Home
			result = DoHome(gb, reply, error);
			break;

		case 30: // Z probe/manually set at a position and set that as point P
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}
			if (reprap.GetMove()->IsDeltaMode() && !AllAxesAreHomed())
			{
				reply.copy("Must home a delta printer before bed probing\n");
				error = true;
			}
			else
			{
				result = SetSingleZProbeAtAPosition(gb, reply);
			}
			break;

		case 31: // Return the probe value, or set probe variables
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}
			result = SetPrintZProbe(gb, reply);
			break;

		case 32: // Probe Z at multiple positions and generate the bed transform
			result = SetBedEquationWithProbe(gb, reply);
			break;

		case 90: // Absolute coordinates
			// DC 2014-07-21 we no longer change the extruder settings in response to G90/G91 commands
			//drivesRelative = false;
			axesRelative = false;
			break;

		case 91: // Relative coordinates
			// DC 2014-07-21 we no longer change the extruder settings in response to G90/G91 commands
			//drivesRelative = true; // Non-axis movements (i.e. extruders)
			axesRelative = true;   // Axis movements (i.e. X, Y and Z)
			break;

		case 92: // Set position  // TODO: Make this report position for no arguments?  Or leave that to M114?
			result = SetPositions(gb);
			break;

		default:
			error = true;
			reply.printf("invalid G Code: %s\n", gb->Buffer());
	}
	if (result)
	{
		HandleReply(gb, error, reply.Pointer());
	}
	return result;
}

bool GCodes::HandleMcode(GCodeBuffer* gb)
{
	bool result = true;
	bool error = false;
	char replyBuffer[LONG_STRING_LENGTH];
	StringRef reply(replyBuffer, ARRAY_SIZE(replyBuffer));
	reply.Clear();

	int code = gb->GetIValue();
	if (simulating && (code < 20 || code > 37) && code != 82 && code != 83 && code != 111 && code != 105 && code != 122
			&& code != 999)
	{
		HandleReply(gb, false, "");
		return true;                    // we don't yet simulate most M codes
	}

	switch (code)
	{
		case 0: // Stop
		case 1: // Sleep
			// Wait for all moves to finish first
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			// If Marlin is emulated and M1 is called, run M25 (Pause) instead
			if (code == 1 && (gb == serialGCode || gb == telnetGCode) && platform->Emulating() == marlin)
			{
				if (reprap.GetPrintMonitor()->IsPrinting() && IsRunning())
				{
					gb->Put("M25\n", 4);
					return false;
				}
			}

			// Call stop.g or sleep.g to allow users to execute custom actions before everything stops
			// This will also guarantee that movement has stopped and that no other macro is running
			if (!DoFileMacro(gb, (code == 0) ? STOP_G : SLEEP_G))
				return false;

			{
				if (code == 0)
				{
					// M0 puts each drive into idle state
					platform->SetDrivesIdle();
				}
				else
				{
					// M1 disables them entirely
					DisableDrives();
				}

				// Switch off heaters only if the 'H' parameter is not passed or its value is zero
				if (!gb->Seen('H') || gb->GetIValue() == 0)
				{
					Tool* tool = reprap.GetCurrentTool();
					if(tool != nullptr)
					{
						reprap.StandbyTool(tool->Number());
					}

					reprap.GetHeat()->SwitchOffAll();
				}

				// In case a print is paused, send a response
				if (IsPaused())
				{
					reply.copy("Print cancelled\n");
				}

				// Reset everything
				CancelPrint();
			}
			break;

		case 3: // Spin spindle
			if (reprap.GetRoland()->Active())
			{
				if(gb->Seen('S'))
				{
					result = reprap.GetRoland()->ProcessSpindle(gb->GetFValue());
				}
			}
			break;

		case 18: // Motors off
		case 84:
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			{
				bool seen = false;
				for(size_t axis=0; axis<AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						axisIsHomed[axis] = false;
						platform->DisableDrive(axis);
						seen = true;
					}
				}

				if (gb->Seen(EXTRUDE_LETTER))
				{
					long int eDrive[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetLongArray(eDrive, eCount);
					for(size_t i = 0; i < eCount; i++)
					{
						seen = true;
						if (eDrive[i] < 0 || eDrive[i] >= (long int)(DRIVES - AXES))
						{
							reply.printf("Invalid extruder number specified: %ld\n", eDrive[i]);
							error = true;
							break;
						}
						platform->DisableDrive(AXES + eDrive[i]);
					}
				}

				if (gb->Seen('S'))
				{
					seen = true;

					float idleTimeout = gb->GetFValue();
					if (idleTimeout < 0.0)
					{
						reply.copy("Idle timeouts cannot be negative!\n");
						error = true;
					}
					else
					{
						reprap.GetMove()->SetIdleTimeout(idleTimeout);
					}
				}

				if (!seen)
				{
					DisableDrives();
				}
				break;
			}

		case 20: // List files on SD card
			{
				OutputBuffer *fileResponse;
				int sparam = (gb->Seen('S')) ? gb->GetIValue() : 0;
				const char* dir = (gb->Seen('P')) ? gb->GetString() : platform->GetGCodeDir();

				if (sparam == 2)
				{
					fileResponse = reprap.GetFilesResponse(dir, true);		// Send the file list in JSON format
				}
				else
				{
					if (!OutputBuffer::Allocate(fileResponse))
					{
						// Cannot allocate an output buffer, try again later
						return false;
					}

					// To mimic the behaviour of the official RepRapPro firmware:
					// If we are emulating RepRap then we print "GCode files:\n" at the start, otherwise we don't.
					// If we are emulating Marlin and the code came via the serial/USB interface, then we don't put quotes around the names and we separate them with newline;
					// otherwise we put quotes around them and separate them with comma.
					if (platform->Emulating() == me || platform->Emulating() == reprapFirmware)
					{
						fileResponse->copy("GCode files:\n");
					}

					bool encapsulateList = ((gb != serialGCode && gb != telnetGCode) || platform->Emulating() != marlin);
					FileInfo fileInfo;
					if (platform->GetMassStorage()->FindFirst(dir, fileInfo))
					{
						// iterate through all entries and append each file name
						do {
							if (encapsulateList)
							{
								fileResponse->catf("%c%s%c%c", FILE_LIST_BRACKET, fileInfo.fileName, FILE_LIST_BRACKET, FILE_LIST_SEPARATOR);
							}
							else
							{
								fileResponse->catf("%s\n", fileInfo.fileName);
							}
						} while (platform->GetMassStorage()->FindNext(fileInfo));

						if (encapsulateList)
						{
							// remove the last separator
							(*fileResponse)[fileResponse->Length() - 1] = '\n';
						}
					}
					else
					{
						fileResponse->cat("NONE\n");
					}
				}

				HandleReply(gb, false, fileResponse);
				return true;
			}

		case 21: // Initialise SD - ignore
			break;

		case 22: // Release SD card
			reply.printf("You cannot eject the SD card on a powered-up %s.\n", platform->GetElectronicsString());
			break;

		case 23: // Set file to print
		case 32: // Select file and start SD print
			if (DoingFileMacro())
			{
				reply.copy("Cannot use M32/M23 in file macros!\n");
				error = true;
			}
			else if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				// Must be ready to use the stack...
				result = false;
			}
			else
			{
				const char* filename = gb->GetUnprecedentedString();
				QueueFileToPrint(filename);
				if (fileToPrint.IsLive())
				{
					reprap.GetPrintMonitor()->StartingPrint(filename);
					if (platform->Emulating() == marlin)
					{
						reply.copy("File opened\nFile selected\n");
					}
				}
				else
				{
					reply.copy("Could not open file for printing!\n");
					error = true;
				}
			}

			if (error || code == 23)
			{
				break;
			}
			// no break otherwise

		case 24: // Print/resume-printing the selected file
			if (IsPausing())
			{
				// Don't continue if the file print is still being paused
				return false;
			}

			// Check if the print can be resumed from this G-code source
			if (DoingFileMacro() && macroSourceGCodeBuffer != gb)
			{
				if (gb == fileGCode)
				{
					error = true;
					reply.copy("M24 is not allowed in G-code files\n");
					break;
				}
				else
				{
					// Wait for the running macro to finish first
					return false;
				}
			}

			// Check if a file to print has been set
			if (!fileToPrint.IsLive())
			{
				reply.copy("Cannot resume print, because no print is in progress!\n");
				error = true;
				break;
			}

			// Is a new file print being started?
			if (IsRunning())
			{
				if (!Push())
				{
					return false;
				}
				fileStackPointer = stackPointer;
				reprap.GetPrintMonitor()->StartedPrint();
			}

			// No - resume a live print
			else
			{
				pauseStatus = PauseStatus::Resuming;

				// Run the resume macro first (if necessary)
				if (doPauseMacro)
				{
					if (!DoFileMacro(gb, RESUME_G))
					{
						return false;
					}
					doPauseMacro = false;
				}

				// Make sure we go back to the right coordinates first
				if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					return false;
				}

				float liveCoordinates[DRIVES];
				reprap.GetMove()->LiveCoordinates(liveCoordinates);

				bool needExtraMove = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if (liveCoordinates[axis] != pauseCoordinates[axis])
					{
						needExtraMove = true;
						break;
					}
				}

				if (needExtraMove)
				{
					for(size_t axis = 0; axis < AXES; axis++)
					{
						moveBuffer[axis] = pauseCoordinates[axis];
					}
					for(size_t eDrive = AXES; eDrive < DRIVES; eDrive++)
					{
						moveBuffer[eDrive] = 0.0;
					}
					// Note we don't set the feedrate here - this may be explicitly done using G1 F... in the resume macro

					moveAvailable = true;
					moveType = 0;
					endStopsToCheck = 0;
					moveFilePos = NO_FILE_POSITION;
					return false;
				}

				// We do overwrite the feedrate here with the previous one of the file print
				moveBuffer[DRIVES] = pauseCoordinates[DRIVES];
				reprap.GetMove()->SetFeedrate(pauseCoordinates[DRIVES]);
				for (size_t i = 0; i < NUM_FANS; ++i)
				{
					platform->SetFanValue(i, pausedFanValues[i]);
				}
				reply.copy("Print resumed\n");
			}

			// Resume printing
			fileBeingPrinted.MoveFrom(fileToPrint);
			fractionOfFilePrinted = -1.0;
			pauseStatus = PauseStatus::NotPaused;
			break;

		case 226: // Gcode Initiated Pause
			if (!IsPausing() && !AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;
			// no break

		case 25: // Pause the print
			if (!IsPausing())
			{
				// Don't allow macro files to pause file prints, this would mess with the file logic
				if (doingFileMacro && macroSourceGCodeBuffer != gb)
				{
					if (gb == fileMacroGCode)
					{
						error = true;
						reply.copy("Pausing prints using macro files is not supported\n");
						break;
					}

					// Wait for other macros to finish first
					return false;
				}

				// Allow this command only if a file is being printed
				if (!fileBeingPrinted.IsLive())
				{
					reply.copy("Cannot pause print, because no file is being printed!\n");
					error = true;
					break;
				}

				// User-initiated pause
				if (code == 25)
				{
					unsigned int skippedMoves;
					FilePosition fPos = reprap.GetMove()->PausePrint(pauseCoordinates, skippedMoves);
					doPauseMacro = !reprap.GetMove()->NoLiveMovement();

					// Rewind the file being printed to the position after the last move to be executed
					if (fPos != NO_FILE_POSITION && fileBeingPrinted.IsLive())
					{
						fileBeingPrinted.Seek(fPos);
					}

					// Deal with the amount of (raw) extrusion we're about to skip
					for (size_t extruder = 0; extruder < DRIVES - AXES; extruder++)
					{
						lastExtruderPosition[extruder] -= pauseCoordinates[extruder + AXES];
					}

					// Take care of the code queue (purge orphaned entries)
					totalMoves -= skippedMoves;
					CodeQueueItem *item = internalCodeQueue, *lastItem = nullptr;
					while (item != nullptr)
					{
						if (item->GetSource() == fileGCode && item->ExecuteAtMove() > totalMoves)
						{
							CodeQueueItem *nextItem = item->Next();
							item->SetNext(internalCodeQueue);
							internalCodeQueue = item;

							if (lastItem != nullptr)
							{
								lastItem->SetNext(nextItem);
							}
							item = nextItem;
						}
						else
						{
							lastItem = item;
							item = item->Next();
						}
					}

					// If there is any move left, clear it now
					if (moveAvailable)
					{
						ClearMove();
					}

					if (reprap.Debug(moduleGcodes))
					{
						platform->MessageF(GENERIC_MESSAGE, "Paused print, file offset=%u\n", fPos);
					}
				}

				// G-code initiated pause
				else
				{
					doPauseMacro = true;

					for (size_t axis = 0; axis < AXES; ++axis)
					{
						pauseCoordinates[axis] = moveBuffer[axis];
					}
					for (size_t extruder = AXES; extruder < DRIVES; ++extruder)
					{
						pauseCoordinates[extruder] = 0.0;
					}
					pauseCoordinates[DRIVES] = moveBuffer[DRIVES];
				}

				fractionOfFilePrinted = fileBeingPrinted.FractionRead();
				fileToPrint.MoveFrom(fileBeingPrinted);
				pauseStatus = PauseStatus::Pausing;
			}

			// The print is being paused. Wait for all moves to stop and record the last fan values
			if (!DoingFileMacro())
			{
				if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					return false;
				}

				for (size_t i = 0; i < NUM_FANS; ++i)
				{
					pausedFanValues[i] = platform->GetFanValue(i);
				}
			}

			// Run the pause macro (if necessary)
			if (doPauseMacro && !DoFileMacro(gb, PAUSE_G))
			{
				return false;
			}

			pauseStatus = PauseStatus::Paused;
			break;

		case 26: // Set SD position
			if (gb->Seen('S'))
			{
				long value = gb->GetLValue();
				if (value < 0)
				{
					reply.copy("SD positions can't be negative!\n");
					error = true;
				}
				else if (fileBeingPrinted.IsLive())
				{
					if (!fileBeingPrinted.Seek(value))
					{
						reply.copy("The specified SD position is invalid!\n");
						error = true;
					}
				}
				else if (fileToPrint.IsLive())
				{
					if (!fileToPrint.Seek(value))
					{
						reply.copy("The specified SD position is invalid!\n");
						error = true;
					}
				}
				else
				{
					reply.copy("Cannot set SD file position, because no print is in progress!\n");
					error = true;
				}
			}
			else
			{
				reply.copy("You must specify the SD position in bytes using the S parameter.\n");
				error = true;
			}
			break;

		case 27: // Report print status - Deprecated
			if (reprap.GetPrintMonitor()->IsPrinting())
			{
				reply.printf("SD printing byte %lu/%lu\n", fileBeingPrinted.Position(), fileBeingPrinted.Length());
			}
			else
			{
				reply.copy("Not SD printing.\n");
			}
			break;

		case 28: // Write to file
			{
				const char* str = gb->GetUnprecedentedString();
				bool ok = OpenFileToWrite(platform->GetGCodeDir(), str, gb);
				if (ok)
				{
					reply.printf("Writing to file: %s\n", str);
				}
				else
				{
					reply.printf("Can't open file %s for writing.\n", str);
					error = true;
				}
			}
			break;

		case 29: // End of file being written; should be intercepted before getting here
			reply.copy("GCode end-of-file being interpreted.\n");
			break;

		case 30: // Delete file
			DeleteFile(gb->GetUnprecedentedString());
			break;

			// For case 32, see case 24

		case 36: // Return file information
			{
				const char* filename = gb->GetUnprecedentedString(true);	// get filename, or nullptr if none provided
				OutputBuffer *fileInfoResponse;
				result = reprap.GetPrintMonitor()->GetFileInfoResponse(filename, fileInfoResponse);
				if (result)
				{
					fileInfoResponse->cat('\n');
					HandleReply(gb, false, fileInfoResponse);
					return true;
				}
			}
			break;

		case 37: // Simulation mode on/off
			if (gb->Seen('S'))
			{
				if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					return false;
				}

				bool wasSimulating = simulating;
				simulating = gb->GetIValue() != 0;
				reprap.GetMove()->Simulate(simulating);

				if (simulating)
				{
					simulationTime = 0.0;
					if (!wasSimulating)
					{
						// Starting a new simulation, so save the current position
						reprap.GetMove()->GetCurrentUserPosition(savedMoveBuffer, 0);
					}
				}
				else if (wasSimulating)
				{
					// Ending a simulation, so restore the position
					SetPositions(savedMoveBuffer);
					reprap.GetMove()->SetFeedrate(savedMoveBuffer[DRIVES]);
				}
			}
			else
			{
				reply.printf("Simulation mode: %s, move time: %.1f sec, other time: %.1f sec\n",
						(simulating) ? "on" : "off", simulationTime, reprap.GetMove()->GetSimulationTime());
			}
			break;

		// M38 - Report SHA1 of file
		// Can take some time. All the actual heavy lifting is in dedicated methods.
		case 38:
		{
			if (isHashing)
			{
				if (advanceHash())
				{
					reportHash();
					return true;
				}
			}
			else
			{
				const char* filename = gb->GetUnprecedentedString(true);

				if (!startHash(filename, gb))
				{
					reply.printf("Cannot find file\n");
					result = true;
					break;
				}
			}

			return false;

		}
		case 42: // Switch I/O pin
			if(gb->Seen('P'))
			{
				int pin = gb->GetIValue();
				if(gb->Seen('S'))
				{
					int val = gb->GetIValue();
					if (!platform->SetOutputPin(pin, val))
					{
						platform->MessageF(GENERIC_MESSAGE, "Setting pin %d to %d is not supported\n", pin, val);
					}
				}
				else
				{
					reply.printf("Pin %d is %s.\n", pin, platform->GetInputPin(pin) ? "high" : "low");
				}
			}
			break;

		case 80: // ATX power on
		case 81: // ATX power off
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			if (code == 81)
			{
				DisableDrives();
			}
			platform->SetAtxPower(code == 80);
			break;

		case 82: // Use absolute extruder positioning
			if (drivesRelative)	// don't reset the absolute extruder position if it was already absolute
			{
				for(size_t extruder = AXES; extruder < DRIVES; extruder++)
				{
					lastExtruderPosition[extruder - AXES] = 0.0;
				}
				drivesRelative = false;
			}
			break;

		case 83: // Use relative extruder positioning
			if (!drivesRelative)	// don't reset the absolute extruder position if it was already relative
			{
				for(size_t extruder = AXES; extruder < DRIVES; extruder++)
				{
					lastExtruderPosition[extruder - AXES] = 0.0;
				}
				drivesRelative = true;
			}
			break;

			// For case 84, see case 18

		case 85: // Set inactive time
			// TODO: put some code in here...
			break;

		case 92: // Set/report steps/mm for some axes
			{
				// Save the current positions as we may need them later
				float positionNow[DRIVES];
				Move *move = reprap.GetMove();
				move->GetCurrentUserPosition(positionNow, 0);

				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if(gb->Seen(axisLetters[axis]))
					{
						platform->SetDriveStepsPerUnit(axis, gb->GetFValue());
						seen = true;
					}
				}

				if(gb->Seen(EXTRUDE_LETTER))
				{
					seen = true;
					float eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetFloatArray(eVals, eCount);
					for(size_t e = 0; e < eCount; e++)
					{
						platform->SetDriveStepsPerUnit(AXES + e, eVals[e]);
					}
				}

				if (seen)
				{
					// On a delta, if we change the drive steps/mm then we need to recalculate the motor positions
					SetPositions(positionNow);
				}
				else
				{
					reply.printf("Steps/mm: X: %.3f, Y: %.3f, Z: %.3f, E: ",
							platform->DriveStepsPerUnit(X_AXIS), platform->DriveStepsPerUnit(Y_AXIS),
							platform->DriveStepsPerUnit(Z_AXIS));
					for(size_t drive = AXES; drive < DRIVES; drive++)
					{
						reply.catf("%.3f", platform->DriveStepsPerUnit(drive));
						if(drive < DRIVES-1)
						{
							reply.cat(":");
						}
					}
					reply.cat("\n");
				}
			}
			break;

		case 98: // Call Macro/Subprogram
			if (gb->Seen('P'))
			{
				result = DoFileMacro(gb, gb->GetString());
			}
			break;

		case 99: // Return from Macro/Subprogram
			result = FileMacroCyclesReturn();
			break;

		case 104: // Deprecated.  This sets the active temperature of every heater of the active tool
			if(gb->Seen('S'))
			{
				float temperature = gb->GetFValue();
				Tool* tool;
				if (gb->Seen('T'))
				{
					int toolNumber = gb->GetIValue();
					toolNumber += gb->GetToolNumberAdjust();
					tool = reprap.GetTool(toolNumber);
				}
				else
				{
					tool = reprap.GetCurrentTool();
					// If no tool is selected, and there is only one tool, set the active temperature for that one
					if (tool == nullptr)
					{
						tool = reprap.GetOnlyTool();
					}
				}
				SetToolHeaters(tool, temperature);
			}
			break;

		case 105: // Get Extruder Temperature / Get Status Message
			{
				OutputBuffer *statusResponse;
				int param = (gb->Seen('S')) ? gb->GetIValue() : 0;
				int seq = (gb->Seen('R')) ? gb->GetIValue() : -1;
				switch (param)
				{
					// case 1 is reserved for future Pronterface versions, see
					// http://reprap.org/wiki/G-code#M105:_Get_Extruder_Temperature

					/* NOTE: The following responses are subject to deprecation */
					case 2:
					case 3:
						statusResponse = reprap.GetLegacyStatusResponse(param, seq);	// send JSON-formatted status response
						if (statusResponse != nullptr)
						{
							statusResponse->cat('\n');
							HandleReply(gb, false, statusResponse);
							return true;
						}
						return false;

					/* This one isn't */
					case 4:
						// Send print status JSON-formatted response
						statusResponse = reprap.GetStatusResponse(3, (gb == auxGCode) ? ResponseSource::AUX : ResponseSource::Generic);
						if (statusResponse != nullptr)
						{
							statusResponse->cat('\n');
							HandleReply(gb, false, statusResponse);
							return true;
						}
						return false;

					default:
						// Report first active tool heater
						reply.copy("T:");
						Tool *currentTool = reprap.GetCurrentTool();
						if (currentTool != nullptr && currentTool->HeaterCount() > 0)
						{
							reply.catf("%.1f", reprap.GetHeat()->GetTemperature(currentTool->Heater(0)));
						}

						// Report bed temperature (if available)
						const int8_t bedHeater = reprap.GetHeat()->GetBedHeater();
						if (bedHeater >= 0)
						{
							reply.catf(" B:%.1f", reprap.GetHeat()->GetTemperature(bedHeater));
						}
						
						// Report current/target temperature for all tool heaters in use
						char ch = ' ';
						float targetTemperature;
						for(size_t heater = min<size_t>(bedHeater, E0_HEATER); heater < reprap.GetToolHeatersInUse(); heater++)
						{
							if (reprap.GetHeat()->GetStatus(heater) == Heat::HeaterStatus::HS_active)
							{
								targetTemperature = reprap.GetHeat()->GetActiveTemperature(heater);
							}
							else
							{
								targetTemperature = reprap.GetHeat()->GetStandbyTemperature(heater);
							}
							reply.catf(" H%d:%.1f/%.1f", heater, reprap.GetHeat()->GetTemperature(heater), targetTemperature);
						}
						reply.cat("\n");
						break;
				}
			}
			break;

		case 106: // Set/report fan values
			if (NUM_FANS == 0)
			{
				reply.copy("No fans have been configured in the firmware!\n");
				error = true;
			}
			else
			{
				bool seen = false;
				int fanNumber = 0;

				if (gb->Seen('P'))		// Choose fan number
				{
					fanNumber = gb->GetIValue();
					if (fanNumber < 0 || fanNumber >= (int)NUM_FANS)
					{
						error = true;
						reply.printf("Fan index %i is invalid, 0..%i are valid\n", fanNumber, NUM_FANS - 1);
						break;
					}
				}

				if (gb->Seen('I'))		// Invert cooling
				{
					platform->SetCoolingInverted(fanNumber, gb->GetIValue() > 0);
					seen = true;
				}

				if (gb->Seen('F'))		// Set PWM frequency
				{
					platform->SetFanPwmFrequency(fanNumber, gb->GetFValue());
					seen = true;
				}

				if (gb->Seen('T'))		// Set thermostatic trigger temperature
				{
					seen = true;
					platform->SetTriggerTemperature(fanNumber, gb->GetFValue());
				}

				if (gb->Seen('H'))		// Set thermostatically-controller heaters
				{
					seen = true;
					long heaters[HEATERS];
					size_t numH = HEATERS;
					gb->GetLongArray(heaters, numH);
					// Note that M106 H-1 disables thermostatic mode. The following code implements that automatically.
					uint16_t hh = 0;
					for (size_t h = 0; h < numH; ++h)
					{
						const int hnum = heaters[h];
						if (hnum >= 0 && hnum < HEATERS)
						{
							hh |= (1 << (unsigned int)hnum);
						}
					}
					platform->SetHeatersMonitored(fanNumber, hh);
				}

				if (gb->Seen('S'))		// Set new fan value - process this after processing 'H' or it may not be acted on
				{
					float f = gb->GetFValue();
					f = min<float>(f, 255.0);
					f = max<float>(f, 0.0);
					seen = true;
					platform->SetFanValue(fanNumber, f);
				}
				else if (gb->Seen('R'))
				{
					seen = true;
					platform->SetFanValue(fanNumber, pausedFanValues[fanNumber]);
				}

				if (!seen)
				{
					reply.printf("Fan%i frequency: %dHz, inverted: %s, ",
							fanNumber,
							(int)(platform->GetFanPwmFrequency(fanNumber)),
							(platform->GetCoolingInverted(fanNumber)) ? "yes" : "no");
					uint16_t hh = platform->GetHeatersMonitored(fanNumber);
					if (hh == 0)
					{
						reply.catf("value: %d%%\n", (int)(platform->GetFanValue(fanNumber) * 100.0));
					}
					else
					{
						reply.catf("trigger: %dC, heaters:", (int)platform->GetTriggerTemperature(fanNumber));
						for (size_t i = 0; i < HEATERS; ++i)
						{
							if ((hh & (1 << i)) != 0)
							{
								reply.catf(" %u", i);
							}
						}
						reply.cat("\n");
					}
				}
			}
			break;

		case 107: // Fan off - deprecated
			if (NUM_FANS == 0)
			{
				reply.copy("No fans have been configured in the firmware!\n");
				error = true;
			}
			else
			{
				int fanNumber = 0;
				if (gb->Seen('P'))
				{
					fanNumber = gb->GetIValue();
					if (fanNumber < 0 || fanNumber > (int)NUM_FANS)
					{
						error = true;
						reply.printf("Fan index %d is invalid, 0..%d are valid\n", fanNumber, NUM_FANS);
						break;
					}
				}

				platform->SetFanValue(fanNumber, 0.0);
				break;
			}

		case 109: // Set Extruder Temperature and Wait - deprecated
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			if(gb->Seen('S'))
			{
				float temperature = gb->GetFValue();
				Tool *tool;
				if (gb->Seen('T'))
				{
					int toolNumber = gb->GetIValue();
					toolNumber += gb->GetToolNumberAdjust();
					tool = reprap.GetTool(toolNumber);
				}
				else
				{
					tool = reprap.GetCurrentTool();
				}
				SetToolHeaters(tool, temperature);
				result = ToolHeatersAtSetTemperatures(tool);
			}
			break;

		case 110: // Set line numbers - line numbers are dealt with in the GCodeBuffer class
			break;

		case 111: // Debug level
			if(gb->Seen('S'))
			{
				bool dbv = (gb->GetIValue() != 0);
				if (gb->Seen('P'))
				{
					reprap.SetDebug(static_cast<Module>(gb->GetIValue()), dbv);
				}
				else
				{
					reprap.SetDebug(dbv);
				}
			}
			else
			{
				reprap.PrintDebug();
			}
			break;

		case 112: // Emergency stop - acted upon in Webserver, but also here in case it comes from USB etc.
			reprap.EmergencyStop();
			Reset();
			reply.copy("Emergency Stop! Reset the controller to continue.\n");
			break;

		case 114: // Deprecated
			GetCurrentCoordinates(reply);
			break;

		case 115: // Print firmware version or set hardware type
			if (gb->Seen('P'))
			{
				platform->SetBoardType((BoardType)gb->GetIValue());
			}
			else
			{
				reply.printf("FIRMWARE_NAME: %s FIRMWARE_VERSION: %s ELECTRONICS: %s DATE: %s\n", NAME, VERSION, platform->GetElectronicsString(), DATE);
			}
			break;

		case 116: // Wait for everything, especially set temperatures
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				return false;

			{
				bool seen = false;

				if (gb->Seen('P'))
				{
					// Wait for the heaters associated with the specified tool to be ready
					int toolNumber = gb->GetIValue();
					toolNumber += gb->GetToolNumberAdjust();
					if (!ToolHeatersAtSetTemperatures(reprap.GetTool(toolNumber)))
					{
						return false;
					}
					seen = true;
				}

				if (gb->Seen('H'))
				{
					// Wait for specified heaters to be ready
					long heaters[HEATERS];
					size_t heaterCount = HEATERS;
					gb->GetLongArray(heaters, heaterCount);
					for(size_t i = 0; i < heaterCount; i++)
					{
						if (!reprap.GetHeat()->HeaterAtSetTemperature(heaters[i]))
						{
							return false;
						}
					}
					seen = true;
				}

				if (gb->Seen('C'))
				{
					// Wait for chamber heater to be ready
					const int8_t chamberHeater = reprap.GetHeat()->GetChamberHeater();
					if (chamberHeater != -1)
					{
						if (!reprap.GetHeat()->HeaterAtSetTemperature(chamberHeater))
						{
							return false;
						}
					}
					seen = true;
				}

				if (!seen)
				{
					// Wait for all heaters to be ready
					result = reprap.GetHeat()->AllHeatersAtSetTemperatures(true);
				}
			}
			break;

		case 117: // Display message
			reprap.SetMessage(gb->GetUnprecedentedString());
			break;

		case 119: // Get Endstop Status
			{
				reply.copy("Endstops - ");
				const char* es;
				char comma = ',';
				for(size_t axis = 0; axis < AXES; axis++)
				{
					switch (platform->Stopped(axis))
					{
						case EndStopHit::lowHit:
							es = "at min stop";
							break;

						case EndStopHit::highHit:
							es = "at max stop";
							break;

						case EndStopHit::lowNear:
							es = "near min stop";
							break;

						case EndStopHit::noStop:
						default:
							es = "not stopped";
					}

					if (axis == AXES - 1)
					{
						comma = ' ';
					}

					reply.catf("%c: %s%c ", axisLetters[axis], es, comma);
				}
				reply.cat("\n");
			}
			break;

		case 120: // Push
			result = Push();
			break;

		case 121: // Pop
			result = Pop();
			break;

		case 122: // Diagnostics
			{
				int val = (gb->Seen('P') ? gb->GetIValue() : 0);
				if (val == 0)
				{
					reprap.Diagnostics();
				}
				else
				{
					platform->DiagnosticTest((DiagnosticTestType)val);
				}
			}
			break;

		case 126: // Valve open
			reply.copy("M126 - valves not yet implemented\n");
			break;

		case 127: // Valve closed
			reply.copy("M127 - valves not yet implemented\n");
			break;

		case 135: // Set PID sample interval
			if(gb->Seen('S'))
			{
				platform->SetHeatSampleTime(gb->GetFValue() * 0.001);  // Value is in milliseconds; we want seconds
			}
			else
			{
				reply.printf("Heat sample time is %.3f seconds.\n", platform->HeatSampleTime());
			}
			break;

		case 140: // Set bed temperature
			{
				int bedHeater;
				if (gb->Seen('H'))
				{
					bedHeater = gb->GetIValue();
					if (bedHeater < 0)
					{
						const int8_t currentBedHeater = reprap.GetHeat()->GetBedHeater();
						if (currentBedHeater != -1)
						{
							reprap.GetHeat()->SwitchOff(currentBedHeater);	// If we're disabling the hot bed, make sure its heater is turned off
						}
						reprap.GetHeat()->SetBedHeater(-1);
						break;
					}
					else if (bedHeater >= HEATERS)
					{
						reply.copy("Invalid heater number!\n");
						error = true;
						break;
					}

					// Check if the heater isn't already in use by a tool
					if (reprap.IsHeaterAssignedToTool(bedHeater))
					{
						reply.printf("Heater %d is already in use by a tool!\n", bedHeater);
						error = true;
						break;
					}

					// Then check if it isn't already used by the chamber heater
					if (reprap.GetHeat()->GetChamberHeater() == bedHeater)
					{
						reply.printf("Heater %d is already in use as chamber heater!\n", bedHeater);
						error = true;
						break;
					}

					// Set new bed heater
					reprap.GetHeat()->SetBedHeater(bedHeater);
				}
				else
				{
					bedHeater = reprap.GetHeat()->GetBedHeater();
					if (bedHeater < 0)
					{
						reply.copy("Hot bed is not present!\n");
						error = true;
						break;
					}
				}

				if(gb->Seen('S'))
				{
					float temperature = gb->GetFValue();
					if (temperature < NEARLY_ABS_ZERO)
					{
						reprap.GetHeat()->SwitchOff(bedHeater);
					}
					else
					{
						reprap.GetHeat()->SetActiveTemperature(bedHeater, temperature);
						reprap.GetHeat()->Activate(bedHeater);
					}
				}
				if(gb->Seen('R'))
				{
					reprap.GetHeat()->SetStandbyTemperature(bedHeater, gb->GetFValue());
				}
			}
			break;

		case 141: // Chamber temperature
			{
				bool seen = false;
				if (gb->Seen('H'))
				{
					seen = true;

					int heater = gb->GetIValue();
					if (heater < 0)
					{
						const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
						if (currentHeater != -1)
						{
							reprap.GetHeat()->SwitchOff(currentHeater);		// Same here. Turn off chamber heater before disabling it
						}
						reprap.GetHeat()->SetChamberHeater(-1);
						break;
					}
					else if (heater < HEATERS)
					{
						// Check if the heater isn't already in use by a tool
						if (reprap.IsHeaterAssignedToTool(heater))
						{
							reply.printf("Heater %d is already in use by a tool!\n", heater);
							error = true;
							break;
						}

						// Then check if the hot bed doesn't already use this one
						if (reprap.GetHeat()->GetBedHeater() == heater)
						{
							reply.printf("Heater %d is already in use by the hot bed!\n", heater);
							error = true;
							break;
						}

						// Set new chamber heater
						reprap.GetHeat()->SetChamberHeater(heater);
					}
					else
					{
						reply.copy("Bad heater number specified!\n");
						error = true;
					}
				}

				if (gb->Seen('S'))
				{
					seen = true;

					const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
					if (currentHeater != -1)
					{
						float temperature = gb->GetFValue();

						if (temperature < NEARLY_ABS_ZERO)
						{
							reprap.GetHeat()->SwitchOff(currentHeater);
						}
						else
						{
							reprap.GetHeat()->SetActiveTemperature(currentHeater, temperature);
							reprap.GetHeat()->Activate(currentHeater);
						}
					}
					else
					{
						reply.copy("No chamber heater has been set up yet!\n");
						error = true;
					}
				}

				if (!seen)
				{
					const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
					if (currentHeater != -1)
					{
						reply.printf("Chamber heater %d is currently at %.1fC\n", currentHeater, reprap.GetHeat()->GetTemperature(currentHeater));
					}
					else
					{
						reply.copy("No chamber heater has been configured yet.\n");
					}
				}
			}
			break;

		case 143: // Maximum hot-end temperature
			if (gb->Seen('S'))
			{
				float limit = gb->GetFValue();
				if (limit > BAD_LOW_TEMPERATURE && limit < BAD_ERROR_TEMPERATURE)
				{
					platform->SetTemperatureLimit(limit);
				}
				else
				{
					reply.copy("Invalid temperature limit\n");
					error = true;
				}
			}
			else
			{
				reply.printf("Temperature limit is %.1fC\n", platform->GetTemperatureLimit());
			}
			break;

		case 144: // Set bed to standby
			{
				const int8_t bedHeater = reprap.GetHeat()->GetBedHeater();
				if (bedHeater < 0)
				{
					reply.copy("Hot bed is not present!\n");
					error = true;
				}
				else
				{
					reprap.GetHeat()->Standby(bedHeater);
				}
			}
			break;

		case 190: // Set bed temperature and wait
			{
				const int8_t bedHeater = reprap.GetHeat()->GetBedHeater();
				if (bedHeater < 0)
				{
					reply.copy("Hot bed is not present!\n");
					error = true;
					break;
				}

				if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					return false;
				}

				if (gb->Seen('S'))
				{
					reprap.GetHeat()->SetActiveTemperature(bedHeater, gb->GetFValue());
					reprap.GetHeat()->Activate(bedHeater);
					result = reprap.GetHeat()->HeaterAtSetTemperature(bedHeater);
				}
			}
			break;

		case 201: // Set/print axis accelerations  FIXME - should these be in /min not /sec ?
			{
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if(gb->Seen(axisLetters[axis]))
					{
						platform->SetAcceleration(axis, gb->GetFValue() * distanceScale);
						seen = true;
					}
				}

				if(gb->Seen(EXTRUDE_LETTER))
				{
					seen = true;
					float eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetFloatArray(eVals, eCount);
					for(size_t e = 0; e < eCount; e++)
					{
						platform->SetAcceleration(AXES + e, eVals[e] * distanceScale);
					}
				}

				if(!seen)
				{
					reply.printf("Accelerations: X: %.1f, Y: %.1f, Z: %.1f, E: ",
							platform->Acceleration(X_AXIS)/distanceScale,
							platform->Acceleration(Y_AXIS)/distanceScale,
							platform->Acceleration(Z_AXIS)/distanceScale);
					for(size_t drive = AXES; drive < DRIVES; drive++)
					{
						reply.catf("%.1f", platform->Acceleration(drive)/distanceScale);
						if(drive < DRIVES-1)
						{
							reply.cat(":");
						}
					}
					reply.cat("\n");
				}
			}
			break;

		case 203: // Set/print maximum feedrates
			{
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if(gb->Seen(axisLetters[axis]))
					{
						platform->SetMaxFeedrate(axis, gb->GetFValue() * distanceScale * SECONDS_TO_MINUTES); // G Code feedrates are in mm/minute; we need mm/sec
						seen = true;
					}
				}

				if(gb->Seen(EXTRUDE_LETTER))
				{
					seen = true;
					float eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetFloatArray(eVals, eCount);
					for(size_t e = 0; e < eCount; e++)
					{
						platform->SetMaxFeedrate(AXES + e, eVals[e] * distanceScale * SECONDS_TO_MINUTES);
					}
				}

				if(!seen)
				{
					reply.printf("Maximum feedrates: X: %.1f, Y: %.1f, Z: %.1f, E: ",
							platform->MaxFeedrate(X_AXIS)/(distanceScale * SECONDS_TO_MINUTES),
							platform->MaxFeedrate(Y_AXIS)/(distanceScale * SECONDS_TO_MINUTES),
							platform->MaxFeedrate(Z_AXIS)/(distanceScale * SECONDS_TO_MINUTES));
					for(size_t drive = AXES; drive < DRIVES; drive++)
					{
						reply.catf("%.1f", platform->MaxFeedrate(drive) / (distanceScale * SECONDS_TO_MINUTES));
						if(drive < DRIVES-1)
						{
							reply.cat(":");
						}
					}
					reply.cat("\n");
				}
			}
			break;

		case 205: //M205 advanced settings:  minimum travel speed S=while printing T=travel only,  B=minimum segment time X= maximum xy jerk, Z=maximum Z jerk
			// This is superseded in this firmware by M codes for the separate types (e.g. M566).
			break;

		case 206:  // Offset axes - Deprecated
			result = OffsetAxes(gb);
			break;

		case 208: // Set/print maximum axis lengths. If there is an S parameter with value 1 then we set the min value, else we set the max value.
			{
				bool setMin = (gb->Seen('S') ? (gb->GetIValue() == 1): false);
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						float value = gb->GetFValue() * distanceScale;
						if (setMin)
						{
							platform->SetAxisMinimum(axis, value);
						}
						else
						{
							platform->SetAxisMaximum(axis, value);
						}
						seen = true;
					}
				}

				if (!seen)
				{
					reply.copy("Axis limits - ");
					char comma = ',';
					for(size_t axis = 0; axis < AXES; axis++)
					{
						if(axis == AXES - 1)
						{
							comma = '\n';
						}
						reply.catf("%c: %.1f min, %.1f max%c ", axisLetters[axis],
								platform->AxisMinimum(axis), platform->AxisMaximum(axis), comma);
					}
				}
			}
			break;

		case 210: // Set/print homing feedrates
			// This is no longer used, but for backwards compatibility we don't report an error
			break;

		case 220: // Set/report speed factor override percentage
			if (gb->Seen('S'))
			{
				float speedFactor = gb->GetFValue() / 100.0;
				if (speedFactor > 0)
				{
					reprap.GetMove()->SetSpeedFactor(speedFactor);
				}
				else
				{
					reply.printf("Invalid speed factor specified.\n");
					error = true;
				}
			}
			else
			{
				reply.printf("Speed factor override: %.1f%%\n", reprap.GetMove()->GetSpeedFactor() * 100.0);
			}
			break;

		case 221: // Set/report extrusion factor override percentage
			{
				int extruder = 0;
				if (gb->Seen('D'))	// D parameter (if present) selects the extruder number
				{
					extruder = gb->GetIValue();
				}

				if (gb->Seen('S'))	// S parameter sets the override percentage
				{
					float extrusionFactor = gb->GetFValue()/100.0;
					if (extruder >= 0 && extruder < (int)(DRIVES - AXES) && extrusionFactor >= 0)
					{
						reprap.GetMove()->SetExtrusionFactor(extruder, extrusionFactor);
					}
				}
				else
				{
					reply.printf("Extrusion factor override for extruder %d: %.1f%%\n", extruder,
							reprap.GetMove()->GetExtrusionFactor(extruder) * 100.0);
				}
			}
			break;

			// For case 226, see case 25

		case 300: // Beep
			if (gb->Seen('P'))
			{
				int ms = gb->GetIValue();
				if (gb->Seen('S'))
				{
					reprap.Beep(gb->GetIValue(), ms);
				}
			}
			break;

		case 301: // Set/report hot end PID values
			SetPidParameters(gb, 1, reply);
			break;

		case 302: // Allow, deny or report cold extrudes
			if (gb->Seen('P'))
			{
				if (gb->GetIValue() > 0)
				{
					reprap.GetHeat()->AllowColdExtrude();
				}
				else
				{
					reprap.GetHeat()->DenyColdExtrude();
				}
			}
			else
			{
				reply.printf("Cold extrudes are %s, use M302 P[1/0] to allow/deny them\n",
						reprap.GetHeat()->ColdExtrude() ? "enabled" : "disabled");
			}
			break;

		case 304: // Set/report heated bed PID values
			{
				const int8_t bedHeater = reprap.GetHeat()->GetBedHeater();
				if (bedHeater >= 0)
				{
					SetPidParameters(gb, bedHeater, reply);
				}
				else
				{
					reply.copy("Hot bed is not present!\n");
					error = true;
				}
			}
			break;

		case 305: // Set/report specific heater parameters
			SetHeaterParameters(gb, reply);
			break;

		case 350: // Set/report microstepping
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}
			{
				// interp is current an int not a bool, because we use special values of interp to set the chopper control register
				int interp = 0;
				if (gb->Seen('I'))
				{
					interp = gb->GetIValue();
				}

				bool seen = false;
				for (size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						seen = true;
						int microsteps = gb->GetIValue();
						if (!platform->SetMicrostepping(axis, microsteps, interp))
						{
							platform->MessageF(GENERIC_MESSAGE, "Drive %c does not support %dx microstepping%s\n",
									axisLetters[axis], microsteps, (interp) ? " with interpolation" : "");
						}
					}
				}

				if (gb->Seen(EXTRUDE_LETTER))
				{
					seen = true;
					long eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetLongArray(eVals, eCount);
					for (size_t e = 0; e < eCount; e++)
					{
						if (!platform->SetMicrostepping(AXES + e, (int)eVals[e], interp))
						{
							platform->MessageF(GENERIC_MESSAGE, "Drive E%u does not support %dx microstepping%s\n",
									e, (int)eVals[e], (interp) ? " with interpolation" : "");
						}
					}
				}

				if (!seen)
				{
					reply.copy("Microstepping - ");
					for (size_t axis = 0; axis < AXES; ++axis)
					{
						bool interp;
						int microsteps = platform->GetMicrostepping(axis, interp);
						reply.catf("%c:%d%s, ", axisLetters[axis], microsteps, (interp) ? "(on)" : "");
					}
					reply.cat("E");
					for (size_t drive = AXES; drive < DRIVES; drive++)
					{
						bool interp;
						int microsteps = platform->GetMicrostepping(drive, interp);
						reply.catf(":%d%s", microsteps, (interp) ? "(on)" : "");
					}
				}
			}
			break;

		case 400: // Wait for current moves to finish
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}
			break;

		case 404: // Filament width and nozzle diameter
		case 407: // Display filament diameter
			{
				bool seen = false;

				if (gb->Seen('N'))
				{
					platform->SetFilamentWidth(gb->GetFValue());
					seen = true;
				}
				if (gb->Seen('D'))
				{
					platform->SetNozzleDiameter(gb->GetFValue());
					seen = true;
				}

				if (!seen)
				{
					reply.printf("Filament width: %.2fmm, nozzle diameter: %.2fmm\n", platform->GetFilamentWidth(), platform->GetNozzleDiameter());
				}
			}
			break;

		case 408: // Report JSON status response
			{
				int type = gb->Seen('S') ? gb->GetIValue() : 0;
				int seq = gb->Seen('R') ? gb->GetIValue() : -1;

				OutputBuffer *statusResponse = nullptr;
				switch (type)
				{
					case 0:
					case 1:
						statusResponse = reprap.GetLegacyStatusResponse(type + 2, seq);
						break;

					case 2:
					case 3:
					case 4:
						statusResponse = reprap.GetStatusResponse(type - 1, (gb == auxGCode) ? ResponseSource::AUX : ResponseSource::Generic);
						break;

					case 5:
						statusResponse = reprap.GetConfigResponse();
						break;
				}

				if (statusResponse != nullptr)
				{
					statusResponse->cat('\n');
					HandleReply(gb, false, statusResponse);
					return true;
				}
			}
			break;

		case 500: // Store parameters in EEPROM
			platform->WriteNvData();
			break;

		case 501: // Load parameters from EEPROM
			platform->ReadNvData();
			if (gb->Seen('S'))
			{
				platform->SetAutoSave(gb->GetIValue() > 0);
			}
			break;

		case 502: // Revert to default "factory settings"
			platform->ResetNvData();
			break;

		case 503: // List variable settings
			{
				// Need a valid output buffer to continue...
				OutputBuffer *configResponse;
				if (!OutputBuffer::Allocate(configResponse))
				{
					// No buffer available, try again later
					return false;
				}

				// Read the entire file
				FileStore *f = platform->GetFileStore(platform->GetSysDir(), platform->GetConfigFile(), false);
				if (f == nullptr)
				{
					error = true;
					reply.copy("Configuration file not found!\n");
				}
				else
				{
					char fileBuffer[FILE_BUFFER_SIZE];
					size_t bytesRead, bytesLeftForWriting = OutputBuffer::GetBytesLeft(configResponse);
					while ((bytesRead = f->Read(fileBuffer, FILE_BUFFER_SIZE)) > 0 && bytesLeftForWriting > 0)
					{
						// Don't write more data than we can process
						if (bytesRead < bytesLeftForWriting)
						{
							bytesLeftForWriting -= bytesRead;
						}
						else
						{
							bytesRead = bytesLeftForWriting;
							bytesLeftForWriting = 0;
						}

						// Write it
						configResponse->cat(fileBuffer, bytesRead);
					}
					f->Close();

					HandleReply(gb, false, configResponse);
					return true;
				}
			}
			break;

		case 540: // Set/report MAC address
			if(gb->Seen('P'))
			{
				SetMACAddress(gb);
			}
			else
			{
				const byte* mac = platform->MACAddress();
				reply.printf("MAC: %x:%x:%x:%x:%x:%x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			}
			break;

		case 550: // Set/report machine name
			if (gb->Seen('P'))
			{
				reprap.SetName(gb->GetString());
			}
			else
			{
				reply.printf("RepRap name: %s\n", reprap.GetName());
			}
			break;

		case 551: // Set password (no option to report it)
			if (gb->Seen('P'))
			{
				reprap.SetPassword(gb->GetString());
			}
			break;

		case 552: // Enable/Disable network and/or Set/Get IP address
			{
				bool seen = false;
				if (gb->Seen('S')) // Has the user turned the network off?
				{
					seen = true;
					if (gb->GetIValue())
					{
						reprap.GetNetwork()->Enable();
					}
					else
					{
						reprap.GetNetwork()->Disable();
					}
				}

				if (gb->Seen('P'))
				{
					seen = true;
					SetEthernetAddress(gb, code);
				}

				if (gb->Seen('R'))
				{
					seen = true;
					reprap.GetNetwork()->SetHttpPort(gb->GetIValue());
				}

				if (!seen)
				{
					const byte *config_ip = platform->IPAddress();
					const byte *actual_ip = reprap.GetNetwork()->IPAddress();
					reply.printf("Network is %s, configured IP address: %d.%d.%d.%d, actual IP address: %d.%d.%d.%d, HTTP port: %d\n",
							reprap.GetNetwork()->IsEnabled() ? "enabled" : "disabled",
							config_ip[0], config_ip[1], config_ip[2], config_ip[3], actual_ip[0], actual_ip[1], actual_ip[2], actual_ip[3],
							reprap.GetNetwork()->GetHttpPort());
				}

			}
			break;

		case 553: // Set/Get netmask
			if (gb->Seen('P'))
			{
				SetEthernetAddress(gb, code);
			}
			else
			{
				const byte *nm = platform->NetMask();
				reply.printf("Net mask: %d.%d.%d.%d\n", nm[0], nm[1], nm[2], nm[3]);
			}
			break;

		case 554: // Set/Get gateway
			if (gb->Seen('P'))
			{
				SetEthernetAddress(gb, code);
			}
			else
			{
				const byte *gw = platform->GateWay();
				reply.printf("Gateway: %d.%d.%d.%d\n", gw[0], gw[1], gw[2], gw[3]);
			}
			break;

		case 555: // Set/report firmware type to emulate
			if (gb->Seen('P'))
			{
				platform->SetEmulating((Compatibility) gb->GetIValue());
			}
			else
			{
				reply.copy("Emulating ");
				switch(platform->Emulating())
				{
					case me:
					case reprapFirmware:
						reply.cat("RepRap Firmware (i.e. in native mode)");
						break;

					case marlin:
						reply.cat("Marlin");
						break;

					case teacup:
						reply.cat("Teacup");
						break;

					case sprinter:
						reply.cat("Sprinter");
						break;

					case repetier:
						reply.cat("Repetier");
						break;

					default:
						reply.catf("Unknown: (%d)", platform->Emulating());
				}
				reply.cat("\n");
			}
			break;

		case 556: // Axis compensation
			if (gb->Seen('S'))
			{
				float value = gb->GetFValue();
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						reprap.GetMove()->SetAxisCompensation(axis, gb->GetFValue() / value);
					}
				}
			}
			else
			{
				reply.printf("Axis compensations - XY: %.5f, YZ: %.5f, ZX: %.5f\n",
						reprap.GetMove()->AxisCompensation(X_AXIS),
						reprap.GetMove()->AxisCompensation(Y_AXIS),
						reprap.GetMove()->AxisCompensation(Z_AXIS));
			}
			break;

		case 557: // Set/report Z probe point coordinates
			if (gb->Seen('P'))
			{
				int point = gb->GetIValue();
				if (point < 0 || (unsigned int)point >= MAX_PROBE_POINTS)
				{
					reply.copy("Z probe point index out of range.\n");
					error = true;
					break;
				}

				bool seen = false;
				if (gb->Seen(axisLetters[X_AXIS]))
				{
					reprap.GetMove()->SetXBedProbePoint(point, gb->GetFValue());
					seen = true;
				}
				if (gb->Seen(axisLetters[Y_AXIS]))
				{
					reprap.GetMove()->SetYBedProbePoint(point, gb->GetFValue());
					seen = true;
				}

				if(!seen)
				{
					reply.printf("Probe point %d - [%.1f, %.1f]\n", point,
							reprap.GetMove()->XBedProbePoint(point),
							reprap.GetMove()->YBedProbePoint(point));
				}
			}
			break;

		case 558: // Set or report Z probe type and for which axes it is used
			{
				bool seenAxes = false, seenType = false, seenParam = false;
				bool zProbeAxes[AXES];
				platform->GetZProbeAxes(zProbeAxes);
				for (size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						zProbeAxes[axis] = (gb->GetIValue() > 0);
						seenAxes = true;
					}
				}
				if (seenAxes)
				{
					platform->SetZProbeAxes(zProbeAxes);
				}

				// We must get and set the Z probe type first before setting the dive height, because different probe types may have different dive heights
				if (gb->Seen('P'))		// probe type
				{
					platform->SetZProbeType(gb->GetIValue());
					seenType = true;
				}

				ZProbeParameters params = platform->GetZProbeParameters();
				if (gb->Seen('H'))		// dive height
				{
					params.diveHeight = gb->GetFValue();
					seenParam = true;
				}

				if (gb->Seen('F'))		// feed rate i.e. probing speed
				{
					params.probeSpeed = gb->GetFValue() * SECONDS_TO_MINUTES;
					seenParam = true;
				}

				if (gb->Seen('T'))		// travel speed to probe point
				{
					params.travelSpeed = gb->GetFValue() * SECONDS_TO_MINUTES;
					seenParam = true;
				}

				if (gb->Seen('S'))		// extra parameter for experimentation
				{
					params.param1 = gb->GetFValue();
					seenParam = true;
				}

				if (gb->Seen('R'))		// extra parameter for experimentation
				{
					params.param2 = gb->GetFValue();
					seenParam = true;
				}

				if (seenParam)
				{
					platform->SetZProbeParameters(params);
				}

				if (!(seenAxes || seenType || seenParam))
				{
					reply.printf("Z Probe type %d, dive height %.1fmm, probe speed %dmm/min, travel speed %dmm/min",
							platform->GetZProbeType(), platform->GetZProbeDiveHeight(),
							(int)(platform->GetZProbeParameters().probeSpeed * MINUTES_TO_SECONDS), (int)(platform->GetZProbeTravelSpeed() * MINUTES_TO_SECONDS));
					if (platform->GetZProbeType() == 5)
					{
						ZProbeParameters params = platform->GetZProbeParameters();
						reply.catf(", parameters %.2f %.2f", params.param1, params.param2);
					}
					reply.cat(", used for these axes:");
					for (size_t axis = 0; axis < AXES; axis++)
					{
						if (zProbeAxes[axis])
						{
							reply.catf(" %c", axisLetters[axis]);
						}
					}
					reply.cat("\n");
				}
			}
			break;

		case 559: // Upload config.g or another gcode file to put in the sys directory
			{
				const char* str = (gb->Seen('P') ? gb->GetString() : platform->GetConfigFile());
				bool ok = OpenFileToWrite(platform->GetSysDir(), str, gb);
				if (ok)
				{
					reply.printf("Writing to file: %s\n", str);
				}
				else
				{
					reply.printf("Can't open file %s for writing.\n", str);
					error = true;
				}
			}
			break;

		case 560: // Upload reprap.htm or another web interface file
			{
				const char* str = (gb->Seen('P') ? gb->GetString() : INDEX_PAGE_FILE);
				bool ok = OpenFileToWrite(platform->GetWebDir(), str, gb);
				if (ok)
				{
					reply.printf("Writing to file: %s\n", str);
				}
				else
				{
					reply.printf("Can't open file %s for writing.\n", str);
					error = true;
				}
			}
			break;

		case 561: // Disable bed transform
			reprap.GetMove()->SetIdentityTransform();
			break;

		case 563: // Define or remove a tool
			ManageTool(gb, reply);
			break;

		case 566: // Set/print minimum feedrates
			{
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if(gb->Seen(axisLetters[axis]))
					{
						platform->SetInstantDv(axis, gb->GetFValue() * distanceScale * SECONDS_TO_MINUTES); // G Code feedrates are in mm/minute; we need mm/sec
						seen = true;
					}
				}

				if(gb->Seen(EXTRUDE_LETTER))
				{
					seen = true;
					float eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetFloatArray(eVals, eCount);
					for(size_t e = 0; e < eCount; e++)
					{
						platform->SetInstantDv(AXES + e, eVals[e] * distanceScale * SECONDS_TO_MINUTES);
					}
				}
				else if(!seen)
				{
					reply.printf("Maximum jerk rates: X: %.1f, Y: %.1f, Z: %.1f, E: ",
							platform->ConfiguredInstantDv(X_AXIS) / (distanceScale * SECONDS_TO_MINUTES),
							platform->ConfiguredInstantDv(Y_AXIS) / (distanceScale * SECONDS_TO_MINUTES),
							platform->ConfiguredInstantDv(Z_AXIS) / (distanceScale * SECONDS_TO_MINUTES));
					for (size_t drive = AXES; drive < DRIVES; drive++)
					{       
						reply.catf("%.1f%c", platform->ConfiguredInstantDv(drive) / (distanceScale * SECONDS_TO_MINUTES),
								(drive < DRIVES - 1) ? ':' : '\n');
					}
				}
			}
			break;

		case 567: // Set/report tool mix ratios
			if (gb->Seen('P'))
			{
				int tNumber = gb->GetIValue();
				Tool* tool = reprap.GetTool(tNumber);
				if (tool != nullptr)
				{
					if (gb->Seen(EXTRUDE_LETTER))
					{
						float eVals[DRIVES - AXES];
						size_t eCount = tool->DriveCount();
						gb->GetFloatArray(eVals, eCount);
						if (eCount != tool->DriveCount())
						{
							reply.printf("Setting mix ratios - wrong number of E drives: %s\n", gb->Buffer());
						}
						else
						{
							tool->DefineMix(eVals);
						}
					}
					else
					{
						reply.printf("Tool %d mix ratios: ", tNumber);
						char sep = ':';
						for(size_t drive = 0; drive < tool->DriveCount(); drive++)
						{
							reply.catf("%.3f%c", tool->GetMix()[drive], sep);
							if (drive >= tool->DriveCount() - 2)
							{
								sep = '\n';
							}
						}
					}
				}
			}
			break;

		case 568: // Turn on/off automatic tool mixing
			if (gb->Seen('P'))
			{
				Tool* tool = reprap.GetTool(gb->GetIValue());
				if (tool != nullptr)
				{
					if (gb->Seen('S'))
					{
						if (gb->GetIValue() != 0)
						{
							tool->TurnMixingOn();
						}
						else
						{
							tool->TurnMixingOff();
						}
					}
				}
			}
			break;

		case 570: // Set/report heater timeout
			if(gb->Seen('S'))
			{
				platform->SetTimeToHot(gb->GetFValue());
			}
			else
			{
				reply.printf("Time allowed to get to temperature: %.1f seconds.\n", platform->TimeToHot());
			}
			break;

		case 571: // Set output on extrude
			if (gb->Seen('S'))
			{
				platform->SetExtrusionAncilliaryPWM(gb->GetFValue());
			}
			else
			{
				reply.printf("Extrusion ancillary PWM: %.3f.\n", platform->GetExtrusionAncilliaryPWM());
			}
			break;

		case 572: // Set/report elastic compensation
			if (gb->Seen('D'))
			{
				// New usage: specify the extruder drive using the D parameter
				size_t extruder = gb->GetIValue();
				if (gb->Seen('S'))
				{
					platform->SetElasticComp(extruder, gb->GetFValue());
				}
				else
				{
					reply.printf("Elastic compensation for extruder %u is %.3f seconds\n", extruder, platform->GetElasticComp(extruder));
				}
			}
			break;

		case 573: // Report heater average PWM
			if (gb->Seen('P'))
			{
				int heater = gb->GetIValue();
				if (heater >= 0 && heater < HEATERS)
				{
					reply.printf("Average heater %d PWM: %.3f.\n", heater, reprap.GetHeat()->GetAveragePWM(heater));
				}
				else
				{
					reply.printf("Invalid heater number: %d\n", heater);
				}
			}
			break;

		case 574: // Set endstop configuration
			{
				bool seen = false;
				bool logicLevel = (gb->Seen('S')) ? (gb->GetIValue() != 0) : true;
				for (size_t axis = 0; axis <= AXES; ++axis)
				{
					const char letter = (axis == AXES) ? EXTRUDE_LETTER : axisLetters[axis];
					if (gb->Seen(letter))
					{
						int ival = gb->GetIValue();
						if (ival >= 0 && ival <= 3)
						{
							platform->SetEndStopConfiguration(axis, (EndStopType) ival, logicLevel);
							seen = true;
						}
					}
				}
				if (!seen)
				{
					reply.copy("Endstop configuration:");
					for (size_t axis = 0; axis < AXES; ++axis)
					{
						EndStopType config;
						bool logic;
						platform->GetEndStopConfiguration(axis, config, logic);
						reply.catf(" %c %s %s %c", axisLetters[axis],
								(config == EndStopType::highEndStop) ? "high end" : (config == EndStopType::lowEndStop) ? "low end" : "none",
								(config == EndStopType::noEndStop) ? "" : (logic) ? " (active high)" : " (active low)",
								(axis == AXES - 1) ? '\n' : ',');
					}
				}
			}
			break;

		case 575: // Set communications parameters
			if (gb->Seen('P'))
			{
				size_t chan = gb->GetIValue();
				if (chan < NUM_SERIAL_CHANNELS)
				{
					bool seen = false;
					if (gb->Seen('B'))
					{
						platform->SetBaudRate(chan, gb->GetIValue());
						seen = true;
					}
					if (gb->Seen('S'))
					{
						uint32_t val = gb->GetIValue();
						platform->SetCommsProperties(chan, val);
						switch (chan)
						{
							case 0:
								serialGCode->SetCommsProperties(val);
								break;
							case 1:
								auxGCode->SetCommsProperties(val);
								break;
							default:
								break;
						}
						seen = true;
					}
					if (!seen)
					{
						uint32_t cp = platform->GetCommsProperties(chan);
						reply.printf("Channel %d: baud rate %d, %s checksum\n", chan, platform->GetBaudRate(chan),
								(cp & 1) ? "requires" : "does not require");
					}
				}
			}
			break;

		case 576: // Set axis/extruder drive mapping
			reply.copy("Not yet implemented!\n");
			break;

		case 577: // Wait until endstop is triggered
			if (gb->Seen('S'))
			{
				// Determine trigger type
				EndStopHit triggerCondition;
				switch (gb->GetIValue())
				{
					case 1:
						triggerCondition = EndStopHit::lowHit;
						break;
					case 2:
						triggerCondition = EndStopHit::highHit;
						break;
					case 3:
						triggerCondition = EndStopHit::lowNear;
						break;
					default:
						triggerCondition = EndStopHit::noStop;
						break;
				}

				// Axis endstops
				for(size_t axis=0; axis<AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						if (platform->Stopped(axis) != triggerCondition)
						{
							result = false;
							break;
						}
					}
				}

				// Extruder drives
				size_t eDriveCount = DRIVES - AXES;
				long eDrives[DRIVES - AXES];
				if (gb->Seen(EXTRUDE_LETTER))
				{
					gb->GetLongArray(eDrives, eDriveCount);
					for(size_t extruder=0; extruder<DRIVES - AXES; extruder++)
					{
						const size_t eDrive = eDrives[extruder] + AXES;
						if (eDrive < AXES || eDrive >= DRIVES)
						{
							reply.copy("Invalid extruder drive specified!\n");
							error = result = true;
							break;
						}

						if (platform->Stopped(eDrive) != triggerCondition)
						{
							result = false;
							break;
						}
					}
				}
			}
			break;

		case 578: // Fire Inkjet bits
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}

			if (gb->Seen('S')) // Need to handle the 'P' parameter too; see http://reprap.org/wiki/G-code#M578:_Fire_inkjet_bits
			{
				platform->Inkjet(gb->GetIValue());
			}
			break;

		case 579: // Scale Cartesian axes (only for Delta configurations)
			{
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						axisScaleFactors[axis] = gb->GetFValue();
						seen = true;
					}
				}

				if (!seen)
				{
					char comma = ',';
					reply.copy("Axis scale factors:");
					for(size_t axis = 0; axis < AXES; axis++)
					{
						if (axis == AXES - 1)
						{
							comma = '\n';
						}
						reply.catf(" %c: %.3f%c", axisLetters[axis], axisScaleFactors[axis], comma);
					}
				}
			}
			break;

		case 580: // (De)Select Roland mill
			if (gb->Seen('R'))
			{
				if (gb->GetIValue())
				{
					reprap.GetRoland()->Activate();
					if (gb->Seen('P'))
					{
						result = reprap.GetRoland()->RawWrite(gb->GetString());
					}
				}
				else
				{
					result = reprap.GetRoland()->Deactivate();
				}
			}
			else
			{
				reply.printf("Roland is %s.\n", reprap.GetRoland()->Active() ? "active" : "inactive");
			}
			break;

		case 665: // Set delta configuration
			result = AllMovesAreFinishedAndMoveBufferIsLoaded();
			if (result)
			{
				float positionNow[DRIVES];
				Move *move = reprap.GetMove();
				move->GetCurrentUserPosition(positionNow, 0);					// get the current position, we may need it later
				DeltaParameters& params = move->AccessDeltaParams();
				bool wasInDeltaMode = params.IsDeltaMode();						// remember whether we were in delta mode
				bool seen = false;

				if (gb->Seen('L'))
				{
					params.SetDiagonal(gb->GetFValue() * distanceScale);
					seen = true;
				}
				if (gb->Seen('R'))
				{
					params.SetRadius(gb->GetFValue() * distanceScale);
					seen = true;
				}
				if (gb->Seen('B'))
				{
					params.SetPrintRadius(gb->GetFValue() * distanceScale);
					seen = true;
				}
				if (gb->Seen('X'))
				{
					// X tower position correction
					params.SetXCorrection(gb->GetFValue());
					seen = true;
				}
				if (gb->Seen('Y'))
				{
					// Y tower position correction
					params.SetYCorrection(gb->GetFValue());
					seen = true;
				}
				if (gb->Seen('Z'))
				{
					// Z tower position correction
					params.SetZCorrection(gb->GetFValue());
					seen = true;
				}

				// The homed height must be done last, because it gets recalculated when some of the other factors are changed
				if (gb->Seen('H'))
				{
					params.SetHomedHeight(gb->GetFValue() * distanceScale);
					seen = true;
				}

				if (seen)
				{
					move->SetCoreXYMode(0);		// CoreXYMode needs to be zero when executing special moves on a delta

					// If we have changed between Cartesian and Delta mode, we need to reset the motor coordinates to agree with the XYZ coordinates.
					// This normally happens only when we process the M665 command in config.g. Also flag that the machine is not homed.
					if (params.IsDeltaMode() != wasInDeltaMode)
					{
						SetPositions(positionNow);
					}
					SetAllAxesNotHomed();
				}
				else
				{
					if (params.IsDeltaMode())
					{
						reply.printf("Diagonal %.2f, delta radius %.2f, homed height %.2f, bed radius %.1f"
								", X %.2f" DEGREE_SYMBOL ", Y %.2f" DEGREE_SYMBOL ", Z %.2f" DEGREE_SYMBOL "\n",
								params.GetDiagonal() / distanceScale, params.GetRadius() / distanceScale,
								params.GetHomedHeight() / distanceScale, params.GetPrintRadius() / distanceScale,
								params.GetXCorrection(), params.GetYCorrection(), params.GetZCorrection());
					}
					else
					{
						reply.printf("Printer is not in delta mode\n");
					}
				}
			}
			break;

		case 666: // Set delta endstop adjustments
			result = AllMovesAreFinishedAndMoveBufferIsLoaded();
			if (result)
			{
				DeltaParameters& params = reprap.GetMove()->AccessDeltaParams();
				bool seen = false;
				if (gb->Seen('X'))
				{
					params.SetEndstopAdjustment(X_AXIS, gb->GetFValue());
					seen = true;
				}
				if (gb->Seen('Y'))
				{
					params.SetEndstopAdjustment(Y_AXIS, gb->GetFValue());
					seen = true;
				}
				if (gb->Seen('Z'))
				{
					params.SetEndstopAdjustment(Z_AXIS, gb->GetFValue());
					seen = true;
				}

				if (seen)
				{
					SetAllAxesNotHomed();
				}
				else
				{
					reply.printf("Endstop adjustments X%.2f Y%.2f Z%.2f\n",
							params.GetEndstopAdjustment(X_AXIS), params.GetEndstopAdjustment(Y_AXIS), params.GetEndstopAdjustment(Z_AXIS));
				}
			}
			break;

		case 667: // Set CoreXY mode
			result = AllMovesAreFinishedAndMoveBufferIsLoaded();
			if (result)
			{
				Move* move = reprap.GetMove();
				bool seen = false;
				float positionNow[DRIVES];
				move->GetCurrentUserPosition(positionNow, 0);					// get the current position, we may need it later
				if (gb->Seen('S'))
				{
					move->SetCoreXYMode(gb->GetIValue());
					seen = true;
				}
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						move->SetCoreAxisFactor(axis, gb->GetFValue());
						seen = true;
					}
				}

				if (seen)
				{
					SetPositions(positionNow);
					SetAllAxesNotHomed();
				}
				else
				{
					reply.printf("Printer mode is %s with axis factors", move->GetGeometryString());
					for (size_t axis = 0; axis < AXES; ++axis)
					{
						reply.catf(" %c:%f", axisLetters[axis], move->GetCoreAxisFactor(axis));
					}
					reply.cat("\n");
				}
			}
			break;

		case 906: // Set/report Motor currents
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}
			{
				bool seen = false;
				for(size_t axis = 0; axis < AXES; axis++)
				{
					if(gb->Seen(axisLetters[axis]))
					{
						platform->SetMotorCurrent(axis, gb->GetFValue());
						seen = true;
					}
				}

				if(gb->Seen(EXTRUDE_LETTER))
				{
					float eVals[DRIVES - AXES];
					size_t eCount = DRIVES - AXES;
					gb->GetFloatArray(eVals, eCount);
					for(size_t e = 0; e < eCount; e++)
					{
						platform->SetMotorCurrent(AXES + e, eVals[e]);
					}
					seen = true;
				}

				if (gb->Seen('I'))
				{
					float idleFactor = gb->GetFValue();
					if (idleFactor >= 0 && idleFactor <= 100.0)
					{
						platform->SetIdleCurrentFactor(idleFactor/100.0);
						seen = true;
					}
				}

				if (!seen)
				{
					reply.copy("Axis currents (mA) - ");
					for (size_t axis = 0; axis < AXES; ++axis)
					{
						reply.catf("%c:%d, ", axisLetters[axis], (int)platform->MotorCurrent(axis));
					}
					reply.cat("E");
					for (size_t drive = AXES; drive < DRIVES; drive++)
					{
						reply.catf("%d%c", (int)platform->MotorCurrent(drive), (drive < DRIVES - 1) ? ':' : ',');
					}
					reply.catf(" idle factor %d%%", (int)(platform->GetIdleCurrentFactor() * 100.0));
				}
			}
			break;

		case 997: // Perform firmware update
			if (!platform->GetMassStorage()->FileExists(platform->GetSysDir(), IAP_FIRMWARE_FILE))
			{
				platform->MessageF(GENERIC_MESSAGE, "Error: Firmware file \"%s\" not found in sys directory\n", IAP_FIRMWARE_FILE);
				break;
			}
			if (!platform->GetMassStorage()->FileExists(platform->GetSysDir(), IAP_UPDATE_FILE))
			{
				platform->MessageF(GENERIC_MESSAGE, "Error: IAP file \"%s\" not found in sys directory\n", IAP_UPDATE_FILE);
				break;
			}

			isFlashing = true;
			if (!DoDwellTime(1.0))
			{
				// wait a second so all HTTP clients are notified
				return false;
			}
			platform->UpdateFirmware();
			isFlashing = false;				// should never get here, but leave this here in case an error has occurred
			break;

		case 998: // Request resend of line
			if (gb->Seen('P'))
			{
				reply.printf("%d\n", gb->GetIValue());
				//resend = true; // FIXME?
			}
			break;

		//****************************
		// These last are M codes only for the cognoscenti

		case 562: // Reset temperature fault - use with great caution
			if (gb->Seen('P'))
			{
				int heater = gb->GetIValue();
				if (heater >= 0 && heater < HEATERS)
				{
					reprap.ClearTemperatureFault(heater);
				}
				else
				{
					reply.copy("Invalid heater number.\n");
					error = true;
				}
			}
			break;

		case 564: // Think outside the box?
			if(gb->Seen('S'))
			{
				limitAxes = (gb->GetIValue() != 0);
			}
			break;

		case 569: // Set/report axis direction
			if (gb->Seen('P'))
			{
				size_t drive = gb->GetIValue();
				if (drive < DRIVES)
				{
					bool seen = false;
					if (gb->Seen('S'))
					{
						platform->SetDirectionValue(drive, gb->GetIValue());
						seen = true;
					}
					if (gb->Seen('R'))
					{
						platform->SetEnableValue(drive, gb->GetIValue() != 0);
						seen = true;
					}
					for (size_t axis = 0; axis < AXES; ++axis)
					{
						if (gb->Seen(axisLetters[axis]))
						{
							platform->SetPhysicalDrive(drive, axis);
							seen = true;
						}
					}
					if (gb->Seen(EXTRUDE_LETTER))
					{
						size_t extruder = gb->GetIValue();
						if (extruder + AXES < DRIVES)
						{
							platform->SetPhysicalDrive(drive, extruder + AXES);
						}
						seen = true;
					}
					if (!seen)
					{
						int physicalDrive = platform->GetPhysicalDrive(drive);
						if (physicalDrive < 0)
						{
							reply.printf("Driver %u is not used\n", drive);
						}
						else
						{
							const char phyDriveLetter = (physicalDrive < (int)AXES) ? axisLetters[physicalDrive] : EXTRUDE_LETTER;
							const int phyDriveNumber = (physicalDrive < (int)AXES) ? 0 : physicalDrive - (int)AXES;
							reply.printf("Driver %u drives the %c%d motor, a %d sends it forwards and a %d enables it\n",
									drive, phyDriveLetter, phyDriveNumber, (int) platform->GetDirectionValue(drive), (int) platform->GetEnableValue(drive));
						}
					}
				}
			}
			break;

		case 999: // Restart after being stopped by error
			result = DoDwellTime(0.5);		// wait half a second to allow the response to be sent back to the web server, otherwise it may retry
			if (result)
			{
				SoftwareResetReason reason = (gb->Seen('P') && StringStartsWith(gb->GetString(), "ERASE"))
					? SoftwareResetReason::erase
					: SoftwareResetReason::user;
				platform->SoftwareReset(reason);			// doesn't return
			}
			break;

		default:
			error = true;
			reply.printf("invalid M Code: %s\n", gb->Buffer());
	}

	if (result)
	{
		HandleReply(gb, error, reply.Pointer());
	}
	return result;
}

bool GCodes::HandleTcode(GCodeBuffer* gb)
{
	if (simulating)                                         // we don't yet simulate any T codes
	{
		HandleReply(gb, false, "");
		return true;
	}

	bool result = true;
	if (strlen(gb->Buffer()) > 1)
	{
		int code = gb->GetIValue();
		code += gb->GetToolNumberAdjust();
		result = ChangeTool(gb, code);
		if (result)
		{
			HandleReply(gb, false, "");
		}
	}
	else
	{
		char reply[SHORT_STRING_LENGTH];
		Tool *tool = reprap.GetCurrentTool();
		if (tool == nullptr)
		{
			strcpy(reply, "No tool is selected.\n");
		}
		else
		{
			snprintf(reply, SHORT_STRING_LENGTH, "Tool %d is selected.\n", tool->Number());
		}
		HandleReply(gb, false, reply);
	}
	return result;
}

bool GCodes::ChangeTool(GCodeBuffer *gb, int newToolNumber)
{
	Tool* oldTool = reprap.GetCurrentTool();
	Tool* newTool = reprap.GetTool(newToolNumber);

	// Allow the tool change macros to be skipped if the 'S0' parameter is passed
	if (gb->Seen('S') && gb->GetIValue() == 0)
	{
		if (oldTool != nullptr)
		{
			// Standby current tool
			reprap.StandbyTool(oldTool->Number());
		}

		if (newTool != nullptr)
		{
			// Select the new one
			reprap.SelectTool(newToolNumber);
		}

		return true;
	}

	// If old and new are the same still follow the sequence -
	// The user may want the macros run.
	switch(toolChangeSequence)
	{
		case 0: // Pre-release sequence for the old tool (if any)
			if (oldTool != nullptr)
			{
				scratchString.printf("tfree%d.g", oldTool->Number());
				if (DoFileMacro(gb, scratchString.Pointer()))
				{
					toolChangeSequence++;
				}
			}
			else
			{
				toolChangeSequence++;
			}
			return false;

		case 1: // Release the old tool (if any)
			if (oldTool != nullptr)
			{
				reprap.StandbyTool(oldTool->Number());
			}
			toolChangeSequence++;
			return false;

		case 2: // Run the pre-tool-change macro cycle for the new tool (if any)
			if (newTool != nullptr)
			{
				scratchString.printf("tpre%d.g", newToolNumber);
				if (DoFileMacro(gb, scratchString.Pointer()))
				{
					toolChangeSequence++;
				}
			}
			else
			{
				toolChangeSequence++;
			}
			return false;

		case 3: // Select the new tool (even if it doesn't exist - that just deselects all tools)
			reprap.SelectTool(newToolNumber);
			toolChangeSequence++;
			return false;

		case 4: // Run the post-tool-change macro cycle for the new tool (if any)
			if (newTool != nullptr)
			{
				scratchString.printf("tpost%d.g", newToolNumber);
				if (DoFileMacro(gb, scratchString.Pointer()))
				{
					toolChangeSequence++;
				}
			}
			else
			{
				toolChangeSequence++;
			}
			return false;

		case 5: // All done
			toolChangeSequence = 0;
			return true;

		default:
			platform->MessageF(GENERIC_MESSAGE, "Error: Tool change - dud sequence number: %d\n", toolChangeSequence);
	}

	toolChangeSequence = 0;
	return true;
}

// Cancel the current SD card print. There must be no live movement when this is called
void GCodes::CancelPrint()
{
	while (internalCodeQueue != nullptr)
	{
		CodeQueueItem *item = internalCodeQueue;
		internalCodeQueue = item->Next();
		item->SetNext(releasedQueueItems);
		releasedQueueItems = item;
	}

	totalMoves = movesCompleted = 0;
	ClearMove();

	pauseStatus = PauseStatus::NotPaused;
	fractionOfFilePrinted = -1.0;
	fileBeingPrinted.Close();

	if (reprap.GetPrintMonitor()->IsPrinting())
	{
		Pop();
		reprap.GetPrintMonitor()->StoppedPrint();
	}
}

// Return true if all the heaters for the specified tool are at their set temperatures
bool GCodes::ToolHeatersAtSetTemperatures(const Tool *tool) const
{
    if (tool != nullptr)
    {
		for(size_t i = 0; i < tool->HeaterCount(); ++i)
		{
			if (!reprap.GetHeat()->HeaterAtSetTemperature(tool->Heater(i)))
			{
				return false;
			}
		}
    }
    return true;
}

// Called by the look-ahead to indicate a new (real) move
void GCodes::MoveQueued()
{
	totalMoves++;
}

// Called by the DDA class to indicate that a move has been completed (called by ISR)
void GCodes::MoveCompleted()
{
	movesCompleted++;
}

bool GCodes::HaveAux() const
{
	return auxDetected;
}

bool GCodes::IsPausing() const
{
	return (pauseStatus == PauseStatus::Pausing);
}

bool GCodes::IsPaused() const
{
	return (pauseStatus == PauseStatus::Paused);
}

bool GCodes::IsResuming() const
{
	return (pauseStatus == PauseStatus::Resuming);
}

bool GCodes::IsRunning() const
{
	return (pauseStatus == PauseStatus::NotPaused);
}

// M38 (SHA1 hash of a file) implementation:
bool GCodes::startHash(const char* filename, GCodeBuffer* source)
{
	if (filename == NULL)
	{
		return false;
	}

	// Get a FileStore object
	fileBeingHashed = platform->GetFileStore(filename, false);

	if (fileBeingHashed == NULL)
	{
		return false;
	}

	isHashing = true;
	hashGCodeSource = source;
	SHA1Reset(&hash);

	return true;
}

bool GCodes::advanceHash()
{
	if (!isHashing)
	{
		platform->Message(HOST_MESSAGE, "Attempting to advance hash but no hash in progress!");
		return false; // I suppose there's no more data...
	}

	if (fileBeingHashed->Position() >= fileBeingHashed->Length())
	{
		return true;
	}

	char buffer[FILE_HASH_BLOCK_SIZE];

	int bytesRead = fileBeingHashed->Read(&buffer[0], FILE_HASH_BLOCK_SIZE);

	SHA1Input(&hash, (const uint8_t*) &buffer[0], FILE_HASH_BLOCK_SIZE);

	return false;
}

void GCodes::reportHash()
{
	SHA1Result(&hash);

	char replyBuffer[LONG_STRING_LENGTH];
	StringRef reply(replyBuffer, ARRAY_SIZE(replyBuffer));
	reply.Clear();

	for (uint8_t i = 0; i < 5; i++) {
		reply.catf("%x", hash.Message_Digest[i]);
	}

	reply.catf("\n");

	HandleReply(hashGCodeSource, false, reply.Pointer());

	isHashing = false;
}
//*************************************************************************************

// This class is used to ensure codes are executed in the right order and independently from the look-ahead queue.

CodeQueueItem::CodeQueueItem(CodeQueueItem *n) : codeLength(0), source(nullptr), next(n)
{
}

void CodeQueueItem::Init(GCodeBuffer *gb, unsigned int executeAtMove)
{
	moveToExecute = executeAtMove;
	next = nullptr;

	codeLength = strlen(gb->Buffer());
	if (codeLength >= ARRAY_SIZE(code))
	{
		reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Error: Invalid string passed to code queue initialiser\n");
		code[0] = 0;
		codeLength = 0;
		source = nullptr;
	}
	else
	{
		strncpy(code, gb->Buffer(), codeLength);
		code[codeLength] = 0;
		source = gb;
	}
}

// vim: ts=4:sw=4
