/*
 * Scanner.h
 *
 *  Created on: 21 Mar 2017
 *      Author: Christian
 */

#ifndef SRC_SCANNER_H_
#define SRC_SCANNER_H_

#include "RepRapFirmware.h"
#include "GCodes/GCodeBuffer.h"

#if SUPPORT_SCANNER

const size_t ScanBufferSize = 512;						// Buffer for incoming codes and upload chunks

enum class ScannerState: char
{
	Disconnected = 'D',
	Idle = 'I',
	Scanning = 'S',
	Uploading = 'U'
};

class Scanner
{
public:
	friend class GCodes;

	Scanner(Platform *p) : platform(p) { }
	void Init();
	void Exit();
	void Spin();

	bool IsEnabled() const { return enabled; }			// Is the usage of a 3D scanner enabled?
	void Enable();										// Enable 3D scanner extension

	bool IsRegistered() const;							// Is the 3D scanner registered and ready to use?
	void Register();									// Register a 3D scanner
	// External scanners are automatically unregistered when the main port (USB) is closed

	void StartScan(const char *filename);				// Start a new 3D scan
	void CancelScan();									// Cancel the 3D scan if it is in progress

	bool DoingGCodes() const { return doingGCodes; }	// Has the scanner run any G-codes since the last state transition?
	const char GetStatusCharacter() const;				// Returns the status char for the status response
	float GetProgress() const;							// Returns the progress of the current action

private:
	GCodeBuffer *serialGCode;
	void SetGCodeBuffer(GCodeBuffer *gb);

	void SetState(const ScannerState s);
	void ProcessCommand();

	Platform *platform;
	float longWait;

	bool enabled;
	bool doingGCodes;
	ScannerState state;

	char buffer[ScanBufferSize];
	size_t bufferPointer;

	float scanProgress;

	size_t uploadSize, uploadBytesLeft;
	FileStore *fileBeingUploaded;
};

inline const char Scanner::GetStatusCharacter() const { return static_cast<char>(state); }
inline bool Scanner::IsRegistered() const { return (state != ScannerState::Disconnected); }
inline void Scanner::SetGCodeBuffer(GCodeBuffer *gb) { serialGCode = gb; }

#endif

#endif
