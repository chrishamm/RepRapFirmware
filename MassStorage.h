#ifndef FILESTORE_H
#define FILESTORE_H

#include "SD_HSMCI.h"

const size_t FILE_BUFFER_SIZE = 256;
const size_t MAX_FILES = 10;					// Must be large enough to handle the max number of simultaneous web requests + files being printed
const size_t FILENAME_LENGTH = 100;


// The following class handle input from, and output to, files.

typedef uint32_t FilePosition;
const FilePosition NO_FILE_POSITION = 0xFFFFFFFF;

struct FileInfo
{
	bool isDirectory;
	unsigned long size;
	uint8_t day;
	uint8_t month;
	uint16_t year;
	char fileName[FILENAME_LENGTH];
};

class Platform;

class MassStorage
{
	public:
		bool FindFirst(const char *directory, FileInfo &file_info);
		bool FindNext(FileInfo &file_info);
		const char* GetMonthName(const uint8_t month);
		const char* CombineName(const char* directory, const char* fileName);
		bool Delete(const char* directory, const char* fileName);
		bool MakeDirectory(const char *parentDir, const char *dirName);
		bool MakeDirectory(const char *directory);
		bool Rename(const char *oldFilename, const char *newFilename);
		bool FileExists(const char *file) const;
		bool FileExists(const char* directory, const char *fileName) const;
		bool DirectoryExists(const char *path) const;
		bool DirectoryExists(const char* directory, const char* subDirectory);

		friend class Platform;

	protected:

		MassStorage(Platform* p);
		void Init();

	private:

		Platform* platform;
		FATFS fileSystem;
		DIR *findDir;
		char combinedName[FILENAME_LENGTH];
};

class FileStore
{
	public:
		bool Read(char& b);								// Read 1 byte
		int Read(char* buf, unsigned int nBytes);		// Read a block of nBytes length
		bool Write(char b);								// Write 1 byte
		bool Write(const char *s, unsigned int len);	// Write a block of len bytes
		bool Write(const char* s);						// Write a string
		bool Close();									// Shut the file and tidy up
		FilePosition Position() const;					// Get the current file position
		bool Seek(FilePosition pos);					// Jump to pos in the file
		bool GoToEnd();									// Position the file at the end (so you can write on the end).
		FilePosition Length() const;					// File size in bytes
		float FractionRead() const;						// How far in we are (in per cent)
		void Duplicate();								// Create a second reference to this file
		bool Flush();									// Write remaining buffer data

		static float GetAndClearLongestWriteTime();		// Return the longest time it took to write a block to a file, in milliseconds

		friend class Platform;

	protected:
		FileStore(Platform* p);
		void Init();
		bool Open(const char* directory, const char* fileName, bool write);
		bool Open(const char* filePath, bool write);

	private:
		bool inUse;
		byte buf[FILE_BUFFER_SIZE];
		int bufferPointer;
		FilePosition bytesRead;

		bool ReadBuffer();
		bool WriteBuffer();
		bool InternalWriteBlock(const char *s, unsigned int len);

		FIL file;
		Platform* platform;
		bool writing;
		unsigned int lastBufferEntry;
		unsigned int openCount;

		static uint32_t longestWriteTime;
};

#endif

// vim: ts=4:sw=4
