#include "RepRapFirmware.h"

MassStorage::MassStorage(Platform* p) : platform(p)
{
	memset(&fileSystem, 0, sizeof(FATFS));
	findDir = new DIR();
}

void MassStorage::Init()
{
	// Initialize SD MMC stack

	sd_mmc_init();

	const size_t startTime = millis();
	sd_mmc_err_t err;
	do {
		err = sd_mmc_check(0);
		delay_ms(1);
	} while (err != SD_MMC_OK && millis() - startTime < 5000);

	if (err != SD_MMC_OK)
	{
		delay_ms(3000);		// Wait a few seconds so users have a chance to see this

		platform->Message(HOST_MESSAGE, "Cannot initialise the SD card: ");
		switch (err)
		{
			case SD_MMC_ERR_NO_CARD:
				platform->Message(HOST_MESSAGE, "Card not found\n");
				break;
			case SD_MMC_ERR_UNUSABLE:
				platform->Message(HOST_MESSAGE, "Card is unusable, try another one\n");
				break;
			case SD_MMC_ERR_SLOT:
				platform->Message(HOST_MESSAGE, "Slot unknown\n");
				break;
			case SD_MMC_ERR_COMM:
				platform->Message(HOST_MESSAGE, "General communication error\n");
				break;
			case SD_MMC_ERR_PARAM:
				platform->Message(HOST_MESSAGE, "Illegal input parameter\n");
				break;
			case SD_MMC_ERR_WP:
				platform->Message(HOST_MESSAGE, "Card write protected\n");
				break;
			default:
				platform->MessageF(HOST_MESSAGE, "Unknown (code %d)\n", err);
				break;
		}
		return;
	}

	// Print some card details (optional)

	/*platform->MessageF(HOST_MESSAGE, "SD card detected!\nCapacity: %d\n", sd_mmc_get_capacity(0));
	platform->MessageF(HOST_MESSAGE, "Bus clock: %d\n", sd_mmc_get_bus_clock(0));
	platform->MessageF(HOST_MESSAGE, "Bus width: %d\nCard type: ", sd_mmc_get_bus_width(0));
	switch (sd_mmc_get_type(0))
	{
		case CARD_TYPE_SD | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "SDHC\n");
			break;
		case CARD_TYPE_SD:
			platform->Message(HOST_MESSAGE, "SD\n");
			break;
		case CARD_TYPE_MMC | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "MMC High Density\n");
			break;
		case CARD_TYPE_MMC:
			platform->Message(HOST_MESSAGE, "MMC\n");
			break;
		case CARD_TYPE_SDIO:
			platform->Message(HOST_MESSAGE, "SDIO\n");
			return;
		case CARD_TYPE_SD_COMBO:
			platform->Message(HOST_MESSAGE, "SD COMBO\n");
			break;
		case CARD_TYPE_UNKNOWN:
		default:
			platform->Message(HOST_MESSAGE, "Unknown\n");
			return;
	}*/

	// Mount the file system

	FRESULT mounted = f_mount(0, &fileSystem);
	if (mounted != FR_OK)
	{
		platform->MessageF(HOST_MESSAGE, "Can't mount filesystem 0: code %d\n", mounted);
	}
}

const char* MassStorage::CombineName(const char* directory, const char* fileName)
{
	size_t out = 0;
	size_t in = 0;

	// DC 2015-11-25 Only prepend the directory if the filename does not have an absolute path
	if (directory != nullptr && fileName[0] != '/' && (strlen(fileName) < 3 || !isdigit(fileName[0]) || fileName[1] != ':' || fileName[2] != '/'))
	{
		while (directory[in] != 0 && directory[in] != '\n')
		{
			combinedName[out] = directory[in];
			in++;
			out++;
			if (out >= ARRAY_SIZE(combinedName))
			{
				platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
				out = 0;
			}
		}

		if (in > 0 && directory[in - 1] != '/' && out < ARRAY_UPB(combinedName))
		{
			combinedName[out] = '/';
			out++;
		}
		in = 0;
	}

	while (fileName[in] != 0 && fileName[in] != '\n')
	{
		combinedName[out] = fileName[in];
		in++;
		out++;
		if (out >= ARRAY_SIZE(combinedName))
		{
			platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
			out = 0;
		}
	}
	combinedName[out] = 0;

	return combinedName;
}

// Open a directory to read a file list. Returns true if it contains any files, false otherwise.
bool MassStorage::FindFirst(const char *directory, FileInfo &file_info)
{
	TCHAR loc[FILENAME_LENGTH];

	// Remove the trailing '/' from the directory name
	size_t len = strnlen(directory, ARRAY_UPB(loc));
	if (len == 0)
	{
		loc[0] = 0;
	}
	else if (directory[len - 1] == '/')
	{
		strncpy(loc, directory, len - 1);
		loc[len - 1] = 0;
	}
	else
	{
		strncpy(loc, directory, len);
		loc[len] = 0;
	}

	findDir->lfn = nullptr;
	FRESULT res = f_opendir(findDir, loc);
	if (res == FR_OK)
	{
		FILINFO entry;
		entry.lfname = file_info.fileName;
		entry.lfsize = ARRAY_SIZE(file_info.fileName);

		for(;;)
		{
			res = f_readdir(findDir, &entry);
			if (res != FR_OK || entry.fname[0] == 0) break;
			if (StringEquals(entry.fname, ".") || StringEquals(entry.fname, "..")) continue;

			file_info.isDirectory = (entry.fattrib & AM_DIR);
			file_info.size = entry.fsize;
			uint16_t day = entry.fdate & 0x1F;
			if (day == 0)
			{
				// This can happen if a transfer hasn't been processed completely.
				day = 1;
			}
			file_info.day = day;
			file_info.month = (entry.fdate & 0x01E0) >> 5;
			file_info.year = (entry.fdate >> 9) + 1980;
			if (file_info.fileName[0] == 0)
			{
				strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
			}

			return true;
		}
	}

	return false;
}

// Find the next file in a directory. Returns true if another file has been read.
bool MassStorage::FindNext(FileInfo &file_info)
{
	FILINFO entry;
	entry.lfname = file_info.fileName;
	entry.lfsize = ARRAY_SIZE(file_info.fileName);

	findDir->lfn = nullptr;
	if (f_readdir(findDir, &entry) != FR_OK || entry.fname[0] == 0)
	{
		//f_closedir(findDir);
		return false;
	}

	file_info.isDirectory = (entry.fattrib & AM_DIR);
	file_info.size = entry.fsize;
	uint16_t day = entry.fdate & 0x1F;
	if (day == 0)
	{
		// This can happen if a transfer hasn't been processed completely.
		day = 1;
	}
	file_info.day = day;
	file_info.month = (entry.fdate & 0x01E0) >> 5;
	file_info.year = (entry.fdate >> 9) + 1980;
	if (file_info.fileName[0] == 0)
	{
		strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
	}

	return true;
}

// Month names. The first entry is used for invalid month numbers.
static const char *monthNames[13] = { "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Returns the name of the specified month or '???' if the specified value is invalid.
const char* MassStorage::GetMonthName(const uint8_t month)
{
	return (month <= 12) ? monthNames[month] : monthNames[0];
}

// Delete a file or directory
bool MassStorage::Delete(const char* directory, const char* fileName)
{
	const char* location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
								: fileName;
	if (f_unlink(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't delete file %s\n", location);
		return false;
	}
	return true;
}

// Create a new directory
bool MassStorage::MakeDirectory(const char *parentDir, const char *dirName)
{
	const char* location = platform->GetMassStorage()->CombineName(parentDir, dirName);
	if (f_mkdir(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", location);
		return false;
	}
	return true;
}

bool MassStorage::MakeDirectory(const char *directory)
{
	if (f_mkdir(directory) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", directory);
		return false;
	}
	return true;
}

// Rename a file or directory
bool MassStorage::Rename(const char *oldFilename, const char *newFilename)
{
	if (f_rename(oldFilename, newFilename) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't rename file or directory %s to %s\n", oldFilename, newFilename);
		return false;
	}
	return true;
}

// Check if the specified file exists
bool MassStorage::FileExists(const char *file) const
{
 	FILINFO fil;
 	fil.lfname = nullptr;
	return (f_stat(file, &fil) == FR_OK);
}

bool MassStorage::FileExists(const char *directory, const char *fileName) const
{
	const char *location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
							: fileName;
	return FileExists(location);
}

// Check if the specified directory exists
bool MassStorage::DirectoryExists(const char *path) const
{
 	DIR dir;
 	dir.lfn = nullptr;
	return (f_opendir(&dir, path) == FR_OK);
}

bool MassStorage::DirectoryExists(const char* directory, const char* subDirectory)
{
	return DirectoryExists(CombineName(directory, subDirectory));
}


/*********************************************************************************/

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

// Block read, doesn't use the buffer but invalidates it. Returns -1 if the read process failed
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
