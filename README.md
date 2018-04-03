# A_Simple_File_System

**Fourth and final OS Project for Spring 2018**

Parts to Implement
==================

Required 
-------------

	int create(char* path)
	int delete(char* path)
	int stat(char* path, struct stat* buf)
	int open(char* path)
	int close(int fileID)
	int read(int fileID, void* buffer, int bytes) 6. int write(int fileID, void* buffer, int bytes) 7. struct dirent* readdir(int directoryID)

Extra Credit
-------------

- Additional indirection using multiple indexing layers.  There are points for single indirect and more for double indirect.
	int opendir(char* path)
	int closedir(int directoryID)
	int mkdir(char* path)
	int rmdir(char* path)


Suggested Procedure
=======================

- Download, build and test the stub code, as detailed in subsection 4.1 of the project detail PDF
- Read through the c and h files in the assignment2/src directory of the stub code
- Make a copy of the stub code, write some simple test code to execute filesystem operations on the example filesystem and look at the log to see how and with what parameters the functions were called.
- First write some simple code to test out the functions in block.c to emulate reading and writing
information in ’disk blocks’ to your flat file.
- Design your file system’s architecture in order to store file metadata and index data blocks.
- Start up the the stub code to get a simple file system (SFS) that outputs logging information, and run
some filesystem commands in it to determine which FUSE functions are used by each system call.
