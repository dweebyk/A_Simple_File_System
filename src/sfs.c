/*
  Simple File System

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.

*/

#include "params.h"
#include "block.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

// ------------------------------------------------------------------------------------------------------
// |superBlock (1)| Inodes (128)| Indirect Blocks (192)| Double I. Blocks (1)| data block metadata (56) |
// ------------------------------------------------------------------------------------------------------
// units in () are meassured in disk blocks

#define NUM_NODES 128
#define NODE_STRT 1
#define IBLK_STRT 129
#define DIBLK 321
#define MDATA_STRT 322
#define INDIR_DATA 378
#define DISK_STRT 379
#define DISK_END 29051 
#define VER 987

typedef struct _inode
{
	int node_num;
	mode_t mode;
	int link_count;
	size_t size;
	long access;
	long modify;
	long change;
	int direct[32];
	int single_indirect[64];
	int double_indirect;
	char name[50];
	int fh; //place to start from in the file
} inode;

typedef struct _indirect
{
	int blocks[128]; //list of block numbers of file
} indirect;

typedef struct _indir_data
{
	char indir_blocks[192]; //is the indirect block used or not
	char d_indir_block; //is the one double indirect block used or not
} indir_data;

typedef struct _data_list
{
	//could have made this more efficient with bit-wise operations, but less mistakes this way
	char data[512];
} data_list;

typedef struct _super_block
{
	//info for root stored in superblock
	mode_t mode;
	size_t size;
	long access;
	long modify;
	long change;
	int verify; //is this our filesystem?
	int num_files; //number of current files
	char node_list[NUM_NODES]; //bit-vector to keep track of unsued inode blocks
} superblock;


///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//

int find_d_indirect()
{
	indir_data indir;
	char* block_buff=malloc(BLOCK_SIZE);
	block_read(INDIR_DATA,block_buff);
	memcpy(&indir,block_buff,sizeof(indir_data));
	if(indir.d_indir_block=='0')
	{
		indir.d_indir_block='1';
		memcpy(block_buff,&indir,sizeof(indir_data));
		block_write(INDIR_DATA,block_buff);
		free(block_buff);
		return DIBLK;
	}
	free(block_buff);
	return -1;
}

int find_indirect()
{
	indir_data indir;
	char* block_buff=malloc(BLOCK_SIZE);
	block_read(INDIR_DATA,block_buff);
	memcpy(&indir,block_buff,sizeof(indir_data));
	int i;
	for(i=0;i<192;i++)
	{
		if(indir.indir_blocks[i]=='0')
		{
			indir.indir_blocks[i]='1';
			memcpy(block_buff,&indir,sizeof(indir_data));
			block_write(INDIR_DATA,block_buff);
			//go to it and set all pointers to -1
			block_read(i+IBLK_STRT,block_buff);
			indirect new;
			memcpy(&new,block_buff,sizeof(indirect));
			int j;
			for(j=0;j<128;j++)
			{
				new.blocks[j]=-1;
			}		
			memcpy(block_buff,&new,sizeof(indirect));
			block_write(i+IBLK_STRT,block_buff);
			return i+IBLK_STRT;
		}			
	}
	return -1;
}


int find_direct()
{
	//find and return the number of the first free data disk block
	int k,l,from=-1;
	data_list metadata;
	char* block_buff=malloc(BLOCK_SIZE);
	for(k=0;k<56;k++)
	{
		block_read(k+MDATA_STRT,block_buff);
		memcpy(&metadata,block_buff,sizeof(data_list));
		for(l=0;l<512;l++)
		{
			if(metadata.data[l]=='0')
			{
				metadata.data[l]='1';
				memcpy(block_buff,&metadata,sizeof(data_list));
				block_write(k+MDATA_STRT,block_buff);
				free(block_buff);
				return DISK_STRT+((k+1)*l);
			}
		}	
	}
	free(block_buff);
	return -1;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
void *sfs_init(struct fuse_conn_info *conn)
{
    //fprintf(stderr, "in sfs_init\n");
    log_msg("\nsfs_init()\n");
    
    disk_open(SFS_DATA->diskfile);
    superblock sblock;
    char* block_buff=malloc(BLOCK_SIZE);
    int check=block_read(0,block_buff);
    if(check<=0)
    {
	    //fs is not inited, so init it
	    int i;
	    log_msg("\nfs file not inited\n");
	    sblock.verify=VER;
	    sblock.num_files=0;
	    sblock.access=(long)time(NULL);
	    sblock.change=sblock.access;
	    sblock.modify=sblock.access;
	    sblock.mode=S_IRWXU;
	    for(i=0;i<NUM_NODES;i++)
	    {
		    sblock.node_list[i]='0';
	    }
	    memcpy(block_buff,&sblock,sizeof(superblock));
	    block_write(0,block_buff);
	    data_list metadata;
	    for(i=0;i<512;i++)
	    {
		    metadata.data[i]='0';
	    }
	    memcpy(block_buff,&metadata,sizeof(data_list));
	    for(i=MDATA_STRT;i<INDIR_DATA;i++)
	    {
		    //initialize all metadatas to empty
		block_write(i,block_buff);
	    }
	    indir_data indir;
	    for(i=0;i<192;i++)
	    {
		indir.indir_blocks[i]='0';
	    }
	    indir.d_indir_block='0';
	    memcpy(block_buff,&indir,sizeof(indir_data));
	    block_write(INDIR_DATA,block_buff);

	    log_msg("\nfinished initing fs\n");
    }
    else
    {
    	//see if it is actually our fs
	memcpy(&sblock,block_buff,sizeof(superblock));
	if(sblock.verify!=VER)
	{
		log_msg("\nnot our fs, exiting failure\n");
		exit(EXIT_FAILURE);
	}
	//otherwise it's fine
	log_msg("\nsuccesfully opened fs file\n");
    }
    free(block_buff);
    

    log_conn(conn);
    //log_fuse_context(fuse_get_context());

    return SFS_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void sfs_destroy(void *userdata)
{
	//nothing to do
	//everything gets written as it happens
    log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
    disk_close(SFS_DATA->diskfile);
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
    int retstat = 0;
    char fpath[PATH_MAX];
    log_msg("\nsfs_getattr(path=\"%s\", statbuf=0x%08x)\n",path, statbuf);
    superblock sb;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    if(strcmp(path,"/")==0)
    {
	    //get root info stored in sb
	    statbuf->st_ino=0;
	    statbuf->st_uid=getuid();
	    statbuf->st_gid=getgid();
	    statbuf->st_mode=S_IFDIR|S_IRWXU|S_IRWXG|S_IRWXO;
	    statbuf->st_nlink=1;
	    statbuf->st_size=0; //maybe whole size of the filesystem
	    statbuf->st_blocks=0; //maybe all blocks in use
	    statbuf->st_atime=sb.access;
	    statbuf->st_mtime=sb.modify;
	    statbuf->st_ctime=sb.change;
	    free(block_buff);
	    log_msg("\nfinished getattr for root\n");
	    return retstat;
    }
    int i;
    inode node;
    //find the file
    for(i=0;i<NUM_NODES;i++)
    {
	    if(sb.node_list[i]=='1')
	    {
		log_msg("\nfound an inode\n");
	    	block_read(i+NODE_STRT,block_buff);
	    	memcpy(&node,block_buff,sizeof(inode));
	    	if(strcmp(&path[1],node.name)==0)
	    	{
		    break;
	    	}
	    }
    }
    if(i==NUM_NODES)
    {
	    log_msg("\ncould not find file to getattr\n");
		statbuf->st_ino=129;
		statbuf->st_uid=getuid();
		statbuf->st_gid=getgid();
		statbuf->st_mode=(S_IFREG|S_IRWXU|S_IRWXG|S_IRWXO);
		statbuf->st_nlink=1;
		statbuf->st_size=0;
		statbuf->st_blocks=0;
		statbuf->st_atime=time(NULL);
		statbuf->st_mtime=time(NULL);
		statbuf->st_ctime=time(NULL);
		retstat=-ENOENT;

    }
    else
    {
	//fill stat
	log_msg("\nreading data from inode number %d\n",node.node_num);
	statbuf->st_ino=node.node_num;
    	statbuf->st_uid=getuid();
    	statbuf->st_gid=getgid();
	//statbuf->st_blksize=(blksize_t)512;
   	statbuf->st_mode=(S_IFREG|S_IRWXU|S_IRWXG|S_IRWXO);
   	statbuf->st_nlink=node.link_count;
	statbuf->st_size=node.size;
	float num_blocks=node.size/512.0;
	if(num_blocks-(int)num_blocks!=0)
	{
		num_blocks++;
	}
	statbuf->st_blocks=(blkcnt_t)(num_blocks);
	statbuf->st_atime=node.access;
	statbuf->st_mtime=node.modify;
	statbuf->st_ctime=node.change;
    }
    free(block_buff);

    log_msg("\ngetattr finished\n");
    return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n",path, mode, fi);
    if(strlen(path)>50)
    {
	    //file name too long
	    log_msg("\nfile name too long\n");
	    retstat=-E2BIG;
	    return retstat;
    }
    superblock sb;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    if(sb.num_files==NUM_NODES)
    {
	    //fs is full
	    log_msg("\nfs is full\n");
	    retstat=-ENOSPC;
	    free(block_buff);
	    return retstat;
    }
    int i,pos=0;
    inode node;
    //check to see if there is a file of the same name
    for(i=0;i<NUM_NODES;i++)
    {
	if(sb.node_list[i]=='1')
	{
		block_read(i+NODE_STRT,block_buff);
		memcpy(&node,block_buff,sizeof(inode));
		if(strcmp(node.name,&(path[1]))==0)
		{
			log_msg("\nfile already exists\n");
			free(block_buff);
			retstat=-EEXIST;
			return retstat;
		}
	}	
    }
    for(i=0;i<NUM_NODES;i++)
    {
	    //find empty inode
	    if(sb.node_list[i]=='0')
	    {
		sb.node_list[i]='1';
		pos=i;
		break;
	    }
    }
    //initialize new inode
    inode new;
    new.node_num=pos+1;
    log_msg("\ncreate inode number %d\n",pos+1);
    new.mode=(S_IFREG|S_IRWXU|S_IRWXG|S_IRWXO);
    new.link_count=1;
    new.size=0;
    new.access=(long)time(NULL);
    new.modify=new.access;
    new.change=new.access;
    for(i=0;i<32;i++)
    {
	    new.direct[i]=-1;
    }
    for(i=0;i<64;i++)
    {
	    new.single_indirect[i]=-1;
    }
    new.double_indirect=-1;
    strcpy(new.name,&(path[1]));
    new.fh=0;
    sb.num_files=sb.num_files+1;
    memcpy(block_buff,&new,sizeof(inode));
    block_write(pos+NODE_STRT,block_buff);
    memcpy(block_buff,&sb,sizeof(superblock));
    block_write(0,block_buff);
    free(block_buff);

    log_msg("\nsfs_create finished\n");
    return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("sfs_unlink(path=\"%s\")\n", path);
    
    int i;
    superblock sb;
    inode node;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    //find the file
    for(i=0;i<NUM_NODES;i++)
    {
	if(sb.node_list[i]=='1')
	{
		block_read(i+NODE_STRT,block_buff);
		memcpy(&node,block_buff,sizeof(inode));
		if(strcmp(node.name,&(path[1]))==0)
		{
			sb.node_list[i]='0';
			break;
		}
	}
    }
    if(i==NUM_NODES)
    {
	    log_msg("\ndid not find file\n");
	    retstat=-ENOENT;
	    free(block_buff);
	    return retstat;
    }
    //mark all data disk blocks associated with the file as free
    //direct blocks
    data_list metadata;
    for(i=0;i<32;i++)
    {
	    int block=node.direct[i];
	    if(block!=-1)
	    {
	    	block=block-DISK_STRT; //first,second,third,... data block. block=[0,28671]
	    	int md_block=block/512; //find which of the 56 metadata blocks holds this ones data. md_block=[0,55]
	    	int md_index=block%512; //find index of this block's data in metadata block. md_index=[0,511]
	    	block_read(md_block+MDATA_STRT,block_buff); //get metadata block holding info for this data block
		memcpy(&metadata,block_buff,sizeof(data_list)); 
		metadata.data[md_index]='0';
		memcpy(block_buff,&metadata,sizeof(data_list));
		block_write(md_block+MDATA_STRT,block_buff);
	    }
    }	   
    //single indirect blocks
    indir_data indir; //metadata struct for all indirect blocks
    indirect i_block; //indirect block struct 
    block_read(INDIR_DATA,block_buff);
    memcpy(&indir,block_buff,sizeof(indir_data));
    for(i=0;i<64;i++)
    {
	    int block=node.single_indirect[i];
	    if(block!=-1)
	    {
		    //mark as free in indir block metadata
		    indir.indir_blocks[block-IBLK_STRT]='0';
		    block_read(block,block_buff);
		    memcpy(&i_block,block_buff,sizeof(indirect));
		    //go to that indir block and mark all of it's blocks as free in thier metadata
		    int j;
		    for(j=0;j<128;j++)
		    {
			    int data_block=i_block.blocks[j];
			    if(data_block!=-1)
			    {
				    data_block=data_block-DISK_STRT;
				    int md_block=data_block/512;
				    int md_index=data_block%512;
				    block_read(md_block+MDATA_STRT,block_buff);
				    memcpy(&metadata,block_buff,sizeof(data_list));
				    metadata.data[md_index]='0';
				    memcpy(block_buff,&metadata,sizeof(data_list));
				    block_write(md_block+MDATA_STRT,block_buff);
			    }
		    }
	    }

    }
    //double indirect block
    if(node.double_indirect!=-1)
    {
	indir.d_indir_block=-1;
	indirect d_block;
	block_read(DIBLK,block_buff);
	memcpy(&d_block,block_buff,sizeof(indirect));
	for(i=0;i<128;i++)
	{
		int block=d_block.blocks[i];
		if(block!=-1)
		{
			//go to that indirect block
			indir.indir_blocks[block-IBLK_STRT]='0';
			block_read(block,block_buff);
			memcpy(&i_block,block_buff,sizeof(indirect));
			int j;
			for(j=0;j<128;j++)
			{
				int data_block=i_block.blocks[j];
				if(data_block!=-1)
				{
					data_block=data_block-DISK_STRT;
					int md_block=data_block/512;
					int md_index=data_block%512;
					block_read(md_block+MDATA_STRT,block_buff);
					memcpy(&metadata,block_buff,sizeof(data_list));
					metadata.data[md_index]='0';
					memcpy(block_buff,&metadata,sizeof(data_list));
					block_write(md_block+MDATA_STRT,block_buff);
				}
			}
		}
	}
    }
    memcpy(block_buff,&indir,sizeof(indir_data));
    block_write(INDIR_DATA,block_buff);
    sb.num_files=sb.num_files-1;
    memcpy(block_buff,&sb,sizeof(superblock));
    block_write(0,block_buff);
    free(block_buff);

    log_msg("\nsfs_unlink finished\n");
    return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int sfs_open(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",path, fi);

    int i;
    superblock sb;
    inode node;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    //check for existance of file
    for(i=0;i<NUM_NODES;i++)
    {
	if(sb.node_list[i]=='1')
	{
		block_read(i+NODE_STRT,block_buff);
		memcpy(&node,block_buff,sizeof(inode));
		if(strcmp(node.name,&(path[1]))==0)
		{
			break;
		}
	}
    }
    if(i==NUM_NODES)
    {
	    log_msg("\ndid not find file\n");
	    retstat=-ENOENT;
	    free(block_buff);
	    return retstat;
    }
    //check permissions of file
    //files have all permissions
/*
    mode_t permission=node.mode;
    if(permission^S_IRUSR==0||permission^S_IWUSR==0)
    {
	retstat=-1;
	free(block_buff);
	log_msg("\nfailed open on permissions\n");
	return retstat;
    }
*/
    free(block_buff);

    log_msg("\nopened file\n");
    return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int sfs_release(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_release(path=\"%s\", fi=0x%08x)\n",path, fi);
    //nothing to be done  
    log_msg("\nrelease finished\n");
    return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",path, buf, size, offset, fi);
    int i;
    superblock sb;
    inode node;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    //check for existance of file
    for(i=0;i<NUM_NODES;i++)
    {
	if(sb.node_list[i]=='1')
	{
		block_read(i+NODE_STRT,block_buff);
		memcpy(&node,block_buff,sizeof(inode));
		if(strcmp(node.name,&(path[1]))==0)
		{
			node.access=time(NULL);
			memcpy(block_buff,&node,sizeof(inode));
			block_write(i+NODE_STRT,block_buff);
			break;
		}
	}
    }
    if(i==NUM_NODES)
    {
	    log_msg("\ndid not find file\n");
	    retstat=-ENOENT;
	    free(block_buff);
	    return retstat;
    }
    //read from the inode
    size_t count=0;
    int start_block=((int)offset)/BLOCK_SIZE;
    int start_index=((int)offset)%BLOCK_SIZE;
    int from;
    while(1)
    {
    	//find block to start reading at
    	if(start_block>(32+128*64))
    	{
		//read from double indirect (block #s 8224-24607)
		block_read(node.double_indirect,block_buff);
		indirect indir;
		memcpy(&indir,block_buff,sizeof(indirect));
		int indir_index=indir.blocks[(start_block-8224)/128];
		block_read(indir_index,block_buff);
		memcpy(&indir,block_buff,sizeof(indirect));
		from=indir.blocks[(start_block-8224)%128];
	}
    	else if(start_block>31)
    	{
		//read from single indirects (block #s 32-8223)
		int indir_index=node.single_indirect[(start_block-32)/128];
		block_read(indir_index,block_buff);
		indirect indir;
		memcpy(&indir,block_buff,sizeof(indirect));
		from=indir.blocks[(start_block-32)%128];	
    	}
    	else
    	{
		//read from direct (block #s 0-31)
		from=node.direct[start_block];
    	}
	log_msg("\nreading from block %d\n",from);
	if(from==-1)
	{
		//trying to read further than it owns
		//
		//
		//
		//
		//
		//
	}
	block_read(from,block_buff);
	//read from that block into buffer
	if(count==0)
	{
		log_msg("\nfile size=%d\n",node.size);
		if(size>node.size)
		{
			//trying to read more than what is there	
			memcpy(buf,block_buff,node.size);
			count=node.size;
			log_msg("\ncount is %d\n",count);
			break;
		} 
		else if(size<BLOCK_SIZE)
		{
			//total request is less than a block
			memcpy(buf,&(block_buff[start_index]),size);
			count=size;
			break;
		}
		else
		{
			//total request is larger than a block
			memcpy(buf,&(block_buff[start_index]),BLOCK_SIZE-start_index);
			count+=BLOCK_SIZE-start_index;
			start_block++;
		}
	}
	else
	{
		if(count==node.size/BLOCK_SIZE&&size-count>node.size-count)
		{
			//trying to read more than what is there
			memcpy(&(buf[count]),block_buff,node.size%BLOCK_SIZE);
			count+=node.size%BLOCK_SIZE;
			break;
		}
		else if(size-count<BLOCK_SIZE)
		{
			//less than a block to go
			memcpy(&(buf[count]),block_buff,BLOCK_SIZE-(size-count));
			count+=BLOCK_SIZE-(size-count);
			break;
		}
		else
		{
			//whole block is requested and more to go
			memcpy(&(buf[count]),block_buff,BLOCK_SIZE);
			count+=BLOCK_SIZE;
			start_block++;
		}
	}
    } 
    log_msg("\ncount is %d\n",count); 
    for(;count<size;count++)
    {
	//pad if needed
	buf[count]='\0';
    }
    buf[count-1]='\0';
    log_msg("\ncount is %d\n",count); 
    log_msg("\nbuf=%s\n",buf);
    retstat=count;
    log_msg("\nread finished\n");
    return retstat;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int sfs_write(const char *path, const char *buf, size_t size, off_t offset,struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",path, buf, size, offset, fi);

    //check if file exists
    int i;
    superblock sb;
    inode node;
    char* block_buff=malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));
    //check for existance of file
    for(i=0;i<NUM_NODES;i++)
    {
	if(sb.node_list[i]=='1')
	{
		block_read(i+NODE_STRT,block_buff);
		memcpy(&node,block_buff,sizeof(inode));
		if(strcmp(node.name,&(path[1]))==0)
		{
			node.modify=time(NULL);
			break;
		}
	}
    }
    if(i==NUM_NODES)
    {
	    log_msg("\ndid not find file\n");
	    retstat=-ENOENT;
	    free(block_buff);
	    return retstat;
    }
    log_msg("\nbuf is %s\n",buf);
    size_t count=0;
    int start_block=((int)offset)/BLOCK_SIZE;
    int start_index=((int)offset)%BLOCK_SIZE;
    int from;
    while(count<size)
    {
    	//find block to start writing to
    	if(start_block>(32+128*64))
    	{
		//write to double indirect (block #s 8224-24607)
		if(node.double_indirect==-1)
		{
			//find a double indirect
			node.double_indirect=find_d_indirect();
			if(node.double_indirect==-1)
			{
				//fs is full
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
		}
		block_read(node.double_indirect,block_buff);
		indirect indir;
		memcpy(&indir,block_buff,sizeof(indirect));
		int indir_index=indir.blocks[(start_block-8224)/128];
		if(indir_index==-1)
		{
			//find an indirect
			indir_index=find_indirect();
			if(indir_index==-1)
			{
				//fs is full
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
			indir.blocks[(start_block-8224)/128]=indir_index;
			memcpy(block_buff,&indir,sizeof(indirect));
			block_write(node.double_indirect,block_buff);
		}
		block_read(indir_index,block_buff);
		memcpy(&indir,block_buff,sizeof(indirect));
		from=indir.blocks[(start_block-8224)%128];
		if(from==-1)
		{
			//try to find free block
			from=find_direct();
			if(from==-1)
			{
				//fs is full
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
			indir.blocks[(start_block-32)%128]=from;
			memcpy(block_buff,&indir,sizeof(indirect));
			block_write(indir_index,block_buff);	
		}
	}
    	else if(start_block>31)
    	{
		//write to single indirects (block #s 32-8223)
		int indir_index=node.single_indirect[(start_block-32)/128];
		if(indir_index==-1)
		{
			//find an indirect
			indir_index=find_indirect();
			if(indir_index==-1)
			{
				//fs is full
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
			node.single_indirect[(start_block-32)/128]=indir_index;
		}
		block_read(indir_index,block_buff);
		indirect indir;
		memcpy(&indir,block_buff,sizeof(indirect));
		from=indir.blocks[(start_block-32)%128];	
		if(from==-1)
		{
			//try to find free block
			from=find_direct();
			if(from==-1)
			{
				//fs is full
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
			indir.blocks[(start_block-32)%128]=from;
			memcpy(block_buff,&indir,sizeof(indirect));
			block_write(indir_index,block_buff);	
		}
    	}
    	else
    	{
		//write to directs (block #s 0-31)
		from=node.direct[start_block];
		if(from==-1)
		{
			//try to find free block
			from=find_direct();
			if(from==-1)
			{
				//system out of memory
				memcpy(block_buff,&node,sizeof(inode));
				block_write(i+NODE_STRT,block_buff);
				return -ENOSPC;
			}
			node.direct[start_block]=from;
		}
    	}
	log_msg("\nwriting to block %d\n",from);
	block_read(from,block_buff);
	//write to that block
	if((size-count)>=BLOCK_SIZE)
	{
		//writing at least an entire block
		memcpy(&(block_buff[start_index]),&(buf[count]),BLOCK_SIZE);
		count+=BLOCK_SIZE;
		block_write(from,block_buff);
	}
	else
	{
		//less than a block to write
		memcpy(&(block_buff[start_index]),&(buf[count]),size-count);
		count+=size-count;
		block_write(from,block_buff);
		break;
	}
	start_index=0;
	start_block++;
    }  
    node.size+=count;
    memcpy(block_buff,&node,sizeof(inode));
    block_write(i+NODE_STRT,block_buff);
    retstat=count;

    log_msg("\nwrite finished\n");
    return retstat;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    int retstat = 0;
    log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
   
    
    return retstat;
}


/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int retstat = 0;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);
    
    
    return retstat;
}


/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    
    
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    int retstat = 0;
    
    superblock sb;
    char* block_buff = malloc(BLOCK_SIZE);
    block_read(0,block_buff);
    memcpy(&sb,block_buff,sizeof(superblock));

    int i;
    inode node;
    for(i=0; i<NUM_NODES; i++)
    {
	if(sb.node_list[i] == '1')
	{
	    block_read(i+NODE_STRT, block_buff);
	    memcpy(&node, block_buff, sizeof(inode));
	    if(filler(buf, node.name, NULL, 0) != 0)
	    {
		log_msg("\nBuffer is full!\n");
		return -ENOMEM;
	    }
	}
    }
    free(block_buff);	
   
    return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;

    
    return retstat;
}

struct fuse_operations sfs_oper = {
  .init = sfs_init,
  .destroy = sfs_destroy,

  .getattr = sfs_getattr,
  .create = sfs_create,
  .unlink = sfs_unlink,
  .open = sfs_open,
  .release = sfs_release,
  .read = sfs_read,
  .write = sfs_write,

  .rmdir = sfs_rmdir,
  .mkdir = sfs_mkdir,

  .opendir = sfs_opendir,
  .readdir = sfs_readdir,
  .releasedir = sfs_releasedir
};

void sfs_usage()
{
    fprintf(stderr, "usage:  sfs [FUSE and mount options] diskFile mountPoint\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct sfs_state *sfs_data;
    
    // sanity checking on the command line
    if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
	sfs_usage();

    sfs_data = malloc(sizeof(struct sfs_state));
    if (sfs_data == NULL) {
	perror("main calloc");
	abort();
    }

    // Pull the diskfile and save it in internal data
    sfs_data->diskfile = argv[argc-2];
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;
    
    sfs_data->logfile = log_open();
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    return fuse_stat;
}
