/*
 * Scanner.cpp
 *
 *  Created on: 21 Mar 2017
 *      Author: Christian
 */

#include "Scanner.h"

#if SUPPORT_SCANNER

#include "RepRap.h"
#include "Platform.h"

void Scanner::Init()
{
	longWait = platform->Time();

	enabled = false;
	SetState(ScannerState::Disconnected);
	bufferPointer = 0;
	scanProgress = 0.0f;
}

void Scanner::SetState(const ScannerState s)
{
	doingGCodes = false;
	state = s;
}

void Scanner::Exit()
{
	if (IsEnabled() && (state == ScannerState::Scanning || state == ScannerState::Uploading))
	{
		CancelScan();
	}
}

void Scanner::Spin()
{
	// Is the 3D scanner extension enabled at all and is a device registered?
	if (!IsEnabled() || state == ScannerState::Disconnected)
	{
		platform->ClassReport(longWait);
		return;
	}

	// Check if the device is still present
	if (!SERIAL_MAIN_DEVICE)
	{
		if (state == ScannerState::Scanning || state == ScannerState::Uploading)
		{
			platform->Message(GENERIC_MESSAGE, "Warning: Scanner disconnected while a 3D scan was in progress");
		}
		SetState(ScannerState::Disconnected);

		if (fileBeingUploaded != nullptr)
		{
			fileBeingUploaded->Close();
			fileBeingUploaded = nullptr;
		}

		platform->ClassReport(longWait);
		return;
	}

	// Are we dealing with fast upload over USB?
	if (state == ScannerState::Uploading)
	{
		// Copy incoming scan data from USB to the buffer
		size_t bytesToCopy = min<size_t>(SERIAL_MAIN_DEVICE.available(), ScanBufferSize - bufferPointer);
		bytesToCopy = min<size_t>(bytesToCopy, uploadBytesLeft);

		for(size_t i = 0; i < bytesToCopy; i++)
		{
			char b = static_cast<char>(SERIAL_MAIN_DEVICE.read());
			buffer[bufferPointer++] = b;
		}
		uploadBytesLeft -= bytesToCopy;

		// When this buffer is full or the upload is complete, write the next chunk
		if (uploadBytesLeft == 0 || bufferPointer == ScanBufferSize)
		{
			if (fileBeingUploaded->Write(buffer, bufferPointer))
			{
				if (uploadBytesLeft == 0)
				{
					if (reprap.Debug(moduleScanner))
					{
						platform->MessageF(HTTP_MESSAGE, "Finished uploading %u bytes of scan data\n", uploadSize);
					}

					fileBeingUploaded->Close();
					fileBeingUploaded = nullptr;
					SetState(ScannerState::Idle);
				}

				bufferPointer = 0;
			}
			else
			{
				platform->Message(GENERIC_MESSAGE, "Error: Could not write scan file\n");
				fileBeingUploaded->Close();
				fileBeingUploaded = nullptr;
				SetState(ScannerState::Idle);
			}
		}

		platform->ClassReport(longWait);
		return;
	}

	// Pick up incoming commands only if the GCodeBuffer is ready. The GCodes class will do the processing for us.
	if (serialGCode->GetState() == GCodeState::normal && SERIAL_MAIN_DEVICE.available() > 0)
	{
		char b = static_cast<char>(SERIAL_MAIN_DEVICE.read());
		if (b == '\n' || b == '\r')
		{
			buffer[bufferPointer] = 0;
			ProcessCommand();
			bufferPointer = 0;
		}
		else
		{
			buffer[bufferPointer++] = b;
			if (bufferPointer >= ScanBufferSize)
			{
				platform->Message(GENERIC_MESSAGE, "Error: Scan buffer overflow\n");
				bufferPointer = 0;
			}
		}
	}

	platform->ClassReport(longWait);
}

// Process incoming commands from the scanner board
void Scanner::ProcessCommand()
{
	// Output some info if debugging is enabled
	if (reprap.Debug(moduleScanner))
	{
		platform->MessageF(HTTP_MESSAGE, "Scanner request: '%s'\n", buffer);
	}

	// Register request: M751
	if (StringEquals(buffer, "M751"))
	{
		SERIAL_MAIN_DEVICE.write("OK\n");
		SERIAL_MAIN_DEVICE.flush();

		SetState(ScannerState::Idle);
	}

	// G-Code request: GCODE <CODE>
	else if (StringStartsWith(buffer, "GCODE "))
	{
		doingGCodes = true;
		serialGCode->Put(&buffer[6], bufferPointer - 6);
	}

	// Progress indicator: PROGRESS <PERCENT>
	else if (StringStartsWith(buffer, "PROGRESS "))
	{
		float progress = atof(&buffer[9]);
		scanProgress = constrain<float>(progress, 0.0f, 100.0f);
	}

	// Upload request: UPLOAD <SIZE> <FILENAME>
	else if (StringStartsWith(buffer, "UPLOAD "))
	{
		uploadSize = atoi(&buffer[7]);
		const char *filename = nullptr;
		for(size_t i = 8; i < bufferPointer - 1; i++)
		{
			if (buffer[i] == ' ')
			{
				filename = &buffer[i+1];
				break;
			}
		}

		if (filename != nullptr)
		{
			uploadBytesLeft = uploadSize;
			fileBeingUploaded = platform->GetFileStore(SCANS_DIRECTORY, filename, true);
			if (fileBeingUploaded != nullptr)
			{
				SetState(ScannerState::Uploading);
				if (reprap.Debug(moduleScanner))
				{
					platform->MessageF(HTTP_MESSAGE, "Starting scan upload for file %s (%u bytes total)\n", filename, uploadSize);
				}
			}
		}
		else
		{
			platform->Message(GENERIC_MESSAGE, "Error: Malformed scanner upload request\n");
		}
	}

	// Error message: ERROR msg
	else if (StringStartsWith(buffer, "ERROR"))
	{
		// close pending uploads
		if (fileBeingUploaded != nullptr)
		{
			fileBeingUploaded->Close();
			fileBeingUploaded = nullptr;
		}

		// if this command contains a message, report it
		if (bufferPointer > 6)
		{
			platform->MessageF(GENERIC_MESSAGE, "Error: %s\n", &buffer[6]);
		}

		// reset the state
		SetState(ScannerState::Idle);
	}
}

// Enable the scanner extensions
void Scanner::Enable()
{
	enabled = true;
}

// Register a scanner device
void Scanner::Register()
{
	if (IsRegistered())
	{
		// Don't do anything if a device is already registered
		return;
	}

	SERIAL_MAIN_DEVICE.write("OK\n");
	SERIAL_MAIN_DEVICE.flush();

	SetState(ScannerState::Idle);
}

// Initiate a new scan
void Scanner::StartScan(const char *filename)
{
	if (state != ScannerState::Idle)
	{
		return;
	}

	// Send the command plus filename
	SERIAL_MAIN_DEVICE.write("SCAN ");
	SERIAL_MAIN_DEVICE.write(filename);
	SERIAL_MAIN_DEVICE.write('\n');
	SERIAL_MAIN_DEVICE.flush();

	// In theory it would be good to verify if this succeeds,
	// but the scanner client cannot give feedback (yet)
	scanProgress = 0.0f;
	SetState(ScannerState::Scanning);
}

// Cancel the running 3D scan
void Scanner::CancelScan()
{
	if (state == ScannerState::Disconnected || state == ScannerState::Idle)
	{
		return;
	}

	SERIAL_MAIN_DEVICE.write("CANCEL\n");
	SERIAL_MAIN_DEVICE.flush();

	SetState(ScannerState::Idle);
}

// Return the progress of the current operation
float Scanner::GetProgress() const
{
	switch (state)
	{
		case ScannerState::Scanning:
			return scanProgress;

		case ScannerState::Uploading:
			return ((float)(uploadSize - uploadBytesLeft) / (float)uploadSize) * 100.0f;

		default:
			return 0.0f;
	}
}

#endif
