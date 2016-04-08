// The following class handle input from, and output to, files.

#ifndef FILESTORE_H
#define FILESTORE_H

typedef uint32_t FilePosition;
const FilePosition NO_FILE_POSITION = 0xFFFFFFFF;
const size_t FILE_BUFFER_SIZE = 256;


class FileStore
{
	public:
		bool Read(char& b);									// Read 1 byte
		int Read(char* buf, size_t nBytes);					// Read a block of nBytes length
		bool Write(char b);									// Write 1 byte
		bool Write(const char *s, size_t len);				// Write a block of len bytes
		bool Write(const char* s);							// Write a string
		bool Close();										// Shut the file and tidy up. May be called from ISR
		FilePosition Position() const { return position; }	// Get the current file position
		bool Seek(FilePosition pos);						// Jump to pos in the file
		bool GoToEnd();										// Position the file at the end (so you can write on the end).
		FilePosition Length() const;						// File size in bytes
		float FractionRead() const;							// How far in we are (in per cent)
		void Duplicate();									// Create a second reference to this file
		bool Flush();										// Write remaining buffer data

		static float GetAndClearLongestWriteTime();			// Return the longest time it took to write a block to a file, in milliseconds

		friend class Platform;

	protected:
		FileStore(Platform* p);
		void Init();
		bool Open(const char* directory, const char* fileName, bool write);
		bool Open(const char* filePath, bool write);

	private:
		volatile bool inUse;
		uint32_t buf32[(FILE_BUFFER_SIZE + 3) / 4];		// use 4-byte aligned memory for HSMCI efficiency
		uint8_t *buf;
		size_t bufferLength, bufferPointer;
		FilePosition position;

		bool ReadBuffer();
		bool WriteBuffer();
		bool InternalWriteBlock(const char *s, size_t len);

		FIL file;
		Platform* platform;
		bool writing;
		volatile size_t openCount;
		volatile bool closeRequested;

		static uint32_t longestWriteTime;
};

#endif

// vim: ts=4:sw=4
