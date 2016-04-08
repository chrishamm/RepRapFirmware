#ifndef GCODEBUFFER_H
#define GCODEBUFFER_H

// Small class to hold an individual GCode and provide functions to allow it to be parsed
class GCodeBuffer
{
	public:
		GCodeBuffer(Platform* p, const char* id);
		//const char *Identity() const { return identity; }
		void Init(); 											// Set it up
		void Diagnostics();										// Write some debug info
		bool Put(char c);										// Add a character to the end
		bool Put(const char *str, size_t len);					// Add an entire string
		bool IsEmpty() const;									// Does this buffer contain any code?
		bool Seen(char c);										// Is a character present?
		float GetFValue();										// Get a float after a key letter
		int GetIValue();										// Get an integer after a key letter
		long GetLValue();										// Get a long integer after a key letter
		const char* GetUnprecedentedString(bool optional = false);	// Get a string with no preceding key letter
		const char* GetString();								// Get a string after a key letter
		const void GetFloatArray(float a[], size_t& length);	// Get a :-separated list of floats after a key letter
		const void GetLongArray(long l[], size_t& length);		// Get a :-separated list of longs after a key letter
		const char* Buffer() const;								// What G-Code has been fed into this buffer?
		bool Active() const;									// Is this G-Code buffer still being acted upon?
		void SetFinished(bool f);								// Set the G Code executed (or not)
		const char* WritingFileDirectory() const;				// If we are writing the G Code to a file, where that file is
		void SetWritingFileDirectory(const char* wfd);			// Set the directory for the file to write the GCode in
		int GetToolNumberAdjust() const { return toolNumberAdjust; }
		void SetToolNumberAdjust(int arg) { toolNumberAdjust = arg; }
		void SetCommsProperties(uint32_t arg) { checksumRequired = (arg & 1); }
		bool StartingNewCode() const { return gcodePointer == 0; }

	private:

		enum class GCodeState { idle, executing };
		int CheckSum();											// Compute the checksum (if any) at the end of the G Code
		Platform* platform;										// Pointer to the RepRap's controlling class
		char gcodeBuffer[GCODE_LENGTH];							// The G Code
		const char* identity;									// Where we are from (web, file, serial line etc)
		size_t gcodePointer;									// Index in the buffer
		int readPointer;										// Where in the buffer to read next or -1 if invalid
		bool inComment;											// Are we after a ';' character?
		bool checksumRequired;									// True if we only accept commands with a valid checksum
		GCodeState state;										// State of this GCodeBuffer
		const char* writingFileDirectory;						// If the G Code is going into a file, where that is
		int toolNumberAdjust;									// Internal offset for tool numbers
};


// Get an Int after a G Code letter
inline int GCodeBuffer::GetIValue()
{
	return static_cast<int>(GetLValue());
}

inline const char* GCodeBuffer::Buffer() const
{
	return gcodeBuffer;
}

inline bool GCodeBuffer::Active() const
{
	return (state == GCodeState::executing);
}

inline void GCodeBuffer::SetFinished(bool f)
{
	if (f)
	{
		state = GCodeState::idle;
		gcodeBuffer[0] = 0;
	}
	else
	{
		state = GCodeState::executing;
	}
}

inline const char* GCodeBuffer::WritingFileDirectory() const
{
	return writingFileDirectory;
}

inline void GCodeBuffer::SetWritingFileDirectory(const char* wfd)
{
	writingFileDirectory = wfd;
}

#endif

// vim: ts=4:sw=4
