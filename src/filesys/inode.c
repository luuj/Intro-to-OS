#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK_NUMBER 10
#define INDIRECT_BLOCK_NUMBER 128
#define DOUBLE_INDIRECT_BLOCK_NUMBER 16384

struct inode_disk
  {
    block_sector_t blocks[DIRECT_BLOCK_NUMBER + 2]; 
    bool is_directory;

    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[113];               /* Not used. */
  };

struct indirect_block
{
  block_sector_t blocks[INDIRECT_BLOCK_NUMBER];
};

struct double_indirect_block
{
  struct indirect_block* blocks[INDIRECT_BLOCK_NUMBER];
};

struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL); 
  if (pos < inode->data.length)
  {
    //How many sectors will be needed
    off_t sector_index = pos / BLOCK_SECTOR_SIZE;    

    //Direct blocks
    if (sector_index < DIRECT_BLOCK_NUMBER)
      return inode->data.blocks[sector_index];
    
    //Indirect block/Double indirect block
    if (sector_index < DIRECT_BLOCK_NUMBER + INDIRECT_BLOCK_NUMBER)
    {
      struct indirect_block *indirect_disk_inode;
      indirect_disk_inode = calloc (1, sizeof (struct indirect_block));
      block_read(fs_device, inode->data.blocks[DIRECT_BLOCK_NUMBER], indirect_disk_inode);
      return indirect_disk_inode->blocks[sector_index - DIRECT_BLOCK_NUMBER];;
    }
    else
    {
      struct indirect_block *indirect_disk_inode;
      indirect_disk_inode = calloc (1, sizeof (struct indirect_block));

      struct double_indirect_block *double_indirect_inode;
      double_indirect_inode = calloc (1, sizeof (struct double_indirect_block));

      sector_index -= INDIRECT_BLOCK_NUMBER + DIRECT_BLOCK_NUMBER;

      block_read(fs_device, inode->data.blocks[DIRECT_BLOCK_NUMBER+1], double_indirect_inode);
      block_read(fs_device, (block_sector_t)double_indirect_inode->blocks[sector_index/INDIRECT_BLOCK_NUMBER], indirect_disk_inode);

      return indirect_disk_inode->blocks[sector_index%INDIRECT_BLOCK_NUMBER];
    }
  }
  
  return -1;
}

//Extend the given sector, treating it as an indirect block
static bool create_indirect_block(block_sector_t* sector, size_t sector_count)
{
  struct indirect_block ib;
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t i;

  if (*sector == 0){
    free_map_allocate(1, sector);
    block_write(fs_device, *sector, zeros);
  }
  block_read(fs_device, *sector, &ib);

  for (i=0; i<sector_count; i++)
  {
    block_sector_t* new_sector = &ib.blocks[i];
    if (*new_sector == 0){
      if (!free_map_allocate(1, new_sector))
        return false;
      block_write(fs_device, *new_sector, zeros);
    }
  }

  block_write(fs_device, *sector, &ib);
  return true;
}

//Extend the given sector, treating it as an double-indirect block
static bool create_double_indirect_block(block_sector_t* sector, size_t sector_count)
{
  struct indirect_block ib;
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t i, k;

  if (*sector == 0){
    free_map_allocate(1, sector);
    block_write(fs_device, *sector, zeros);
  }
  block_read(fs_device, *sector, &ib);

  for (i=0; i<DIV_ROUND_UP(sector_count, INDIRECT_BLOCK_NUMBER); i++)
  {
    block_sector_t* new_sector = &ib.blocks[i];
    struct indirect_block new_ib;

    if (*new_sector == 0){
      free_map_allocate(1, new_sector);
      block_write(fs_device, *new_sector, zeros);
    }
    block_read(fs_device, *new_sector, &new_ib);

    for (k=0; k<sector_count; k++)
    {
      block_sector_t* direct_sector = &new_ib.blocks[k];
      if (*direct_sector == 0){
        if (!free_map_allocate(1, direct_sector))
          return false;
        block_write(fs_device, *direct_sector, zeros);
    }

    block_write(fs_device, *new_sector, &new_ib);
    }
  }

  block_write(fs_device, *sector, &ib);
  return true;
}

//Given an inode_disk, extend it so that it can fit more data
static bool extend_inode(struct inode_disk *disk_inode, size_t sectors)
{
  //Count up to direct blocks only
  size_t iteration_count = (sectors < DIRECT_BLOCK_NUMBER) ? sectors : DIRECT_BLOCK_NUMBER;

  //Direct block
  size_t k;
  for (k=0; k<iteration_count; k++)
  {
    if (disk_inode->blocks[k] == 0)
    {
      if (free_map_allocate(1, &disk_inode->blocks[k]))
      {
        static char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, disk_inode->blocks[k], zeros);
      }
      else
        return false;

    if (iteration_count == sectors && k == iteration_count)
      return true;
    }
  }

  //Indirect block
  sectors -= iteration_count;
  iteration_count = (sectors < INDIRECT_BLOCK_NUMBER) ? sectors : INDIRECT_BLOCK_NUMBER;
  if (!create_indirect_block(&disk_inode->blocks[DIRECT_BLOCK_NUMBER], iteration_count))
    return false;
  else if (sectors == iteration_count)
    return true;

  //Double-indirect block
  sectors -= iteration_count;
  iteration_count = (sectors < DOUBLE_INDIRECT_BLOCK_NUMBER) ? sectors : DOUBLE_INDIRECT_BLOCK_NUMBER;
  if (! create_double_indirect_block(&disk_inode->blocks[DIRECT_BLOCK_NUMBER+1], iteration_count))
    return false;

  return true;
}

static void unextend_inode(struct inode *inode, size_t sectors)
{
  size_t num_sectors = (sectors < DIRECT_BLOCK_NUMBER) ? sectors : DIRECT_BLOCK_NUMBER;
  size_t k;

  for (k=0; k<num_sectors; k++)
  {
    free_map_release(inode->data.blocks[k], 1);
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_directory)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;
  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_directory = is_directory;
      
      if (extend_inode(disk_inode, sectors))
      {
        block_write(fs_device, sector, disk_inode);
        success = true;
      }

      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          unextend_inode(inode, bytes_to_sectors(inode->data.length)); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  off_t total_size = size + offset;

  if (inode->deny_write_cnt)
    return 0;
  
  if (byte_to_sector(inode, total_size) == -1u)
  {
    if (extend_inode(&inode->data, bytes_to_sectors(total_size)))
    {
      (&inode->data)->length = total_size;
      block_write(fs_device, inode->sector, &inode->data);
    }
    else
    {
      return bytes_written;
    }
  }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
