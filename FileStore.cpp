#include "RepRapFirmware.h"

FileStore::FileStore(Platform* p) : platform(p)
{
	buf = reinterpret_cast<uint8_t *>(buf32);
}

void FileStore::Init()
{
	bufferLength = bufferPointer = position = 0;
	inUse = false;
	writing = false;
	openCount = 0;
	closeRequested = false;
}

// Open a local file (for example on an SD card).
// This is protected - only Platform can access it.

bool FileStore::Open(const char* directory, const char* fileName, bool write)
{
	const char *location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
							: fileName;
	FRESULT openReturn = f_open(&file, location, write ? (FA_CREATE_ALWAYS | FA_WRITE) : (FA_OPEN_EXISTING | FA_READ));
	if (openReturn != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't open %s to %s, error code %d\n", location, (writing) ? "write" : "read", openReturn);
		return false;
	}

	writing = write;
	inUse = true;
	openCount = 1;
	return true;
}

void FileStore::Duplicate()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to dup a non-open file.\n");
		return;
	}
	irqflags_t flags = cpu_irq_save();
	++openCount;
	cpu_irq_restore(flags);
}

// This may be called from an ISR, in which case we need to defer the close
bool FileStore::Close()
{
	if (inInterrupt())
	{
		if (!inUse)
		{
			return false;
		}

		irqflags_t flags = cpu_irq_save();
		if (openCount > 1)
		{
			--openCount;
		}
		else
		{
			closeRequested = true;
		}
		cpu_irq_restore(flags);
		return true;
	}

	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to close a non-open file.\n");
		return false;
	}

	irqflags_t flags = cpu_irq_save();
	--openCount;
	bool leaveOpen = (openCount != 0);
	cpu_irq_restore(flags);

	if (leaveOpen)
	{
		return true;
	}

	bool ok = true;
	if (writing)
	{
		ok = Flush();
	}

	FRESULT fr = f_close(&file);
	Init();
	return ok && fr == FR_OK;
}

bool FileStore::Seek(FilePosition pos)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to seek on a non-open file.\n");
		return false;
	}
	if (writing)
	{
		WriteBuffer();
	}
	FRESULT fr = f_lseek(&file, pos);
	if (fr == FR_OK)
	{
		bufferPointer = bufferLength = 0;
		position = pos;
		return true;
	}
	return false;
}

bool FileStore::GoToEnd()
{
	return Seek(Length());
}

FilePosition FileStore::Length() const
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to size non-open file.\n");
		return 0;
	}
	return file.fsize;
}

float FileStore::FractionRead() const
{
	FilePosition len = Length();
	if (len <= 0)
	{
		return 0.0;
	}
	return (float)position / (float)len;
}

bool FileStore::ReadBuffer()
{
	FRESULT readStatus = f_read(&file, buf, FILE_BUFFER_SIZE, &bufferLength);	// Read a chunk of file
	if (readStatus != FR_OK)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		bufferLength = bufferPointer = 0;
		return false;
	}

	bufferPointer = 0;
	return true;
}

// Single character read via the buffer
bool FileStore::Read(char& b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return false;
	}

	if (bufferLength == 0 || bufferPointer >= FILE_BUFFER_SIZE)
	{
		if (!ReadBuffer())
		{
			return false;
		}
	}

	if (bufferPointer >= bufferLength)
	{
		b = 0;  // Good idea?
		return false;
	}

	b = (char) buf[bufferPointer];
	bufferPointer++;
	position++;
	return true;
}

// Block read, doesn't use the buffer but invalidates it.
// Returns the number of bytes read or -1 if the read process failed
int FileStore::Read(char* extBuf, size_t nBytes)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return -1;
	}
	position += bufferLength - bufferPointer;
	bufferLength = bufferPointer = 0;

	size_t numBytesRead;
	FRESULT readStatus = f_read(&file, extBuf, nBytes, &numBytesRead);
	if (readStatus)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		return -1;
	}
	position += numBytesRead;

	return numBytesRead;
}

bool FileStore::WriteBuffer()
{
	if (bufferPointer != 0)
	{
		if (!InternalWriteBlock((const char *)buf, bufferPointer))
		{
			return false;
		}
		bufferPointer = 0;
	}
	return true;
}

bool FileStore::Write(char b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write byte to a non-open file.\n");
		return false;
	}
	buf[bufferPointer] = b;
	bufferPointer++;
	if (bufferPointer >= FILE_BUFFER_SIZE)
	{
		return WriteBuffer();
	}
	return true;
}

bool FileStore::Write(const char* b)
{
	return Write(b, strlen(b));
}

// Direct block write that bypasses the buffer. Used when uploading files.
bool FileStore::Write(const char *s, size_t len)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write block to a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return InternalWriteBlock(s, len);
}

bool FileStore::InternalWriteBlock(const char *s, size_t len)
{
	size_t bytesWritten;
	uint32_t time = micros();
	FRESULT writeStatus = f_write(&file, s, len, &bytesWritten);
	time = micros() - time;
	if (time > longestWriteTime)
	{
		longestWriteTime = time;
	}
	if ((writeStatus != FR_OK) || (bytesWritten != len))
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Cannot write to file. Disc may be full (code %d).\n", writeStatus);
		return false;
	}
	position += bytesWritten;
	return true;
}

bool FileStore::Flush()
{
	if (!inUse && !closeRequested)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to flush a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return f_sync(&file) == FR_OK;
}

float FileStore::GetAndClearLongestWriteTime()
{
	float ret = (float)longestWriteTime/1000.0;
	longestWriteTime = 0;
	return ret;
}

uint32_t FileStore::longestWriteTime = 0;

// vim: ts=4:sw=4
