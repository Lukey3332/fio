/*
 * Copyright (C) 2018 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/blkzoned.h>
#include "file.h"
#include "fio.h"
#include "lib/pow2.h"
#include "log.h"
#include "smalloc.h"
#include "verify.h"
#include "zbd.h"

/**
 * zbd_zone_idx - convert an offset into a zone number
 * @f: file pointer.
 * @offset: offset in bytes. If this offset is in the first zone_size bytes
 *	    past the disk size then the index of the sentinel is returned.
 */
static uint32_t zbd_zone_idx(const struct fio_file *f, uint64_t offset)
{
	uint32_t zone_idx;

	if (f->zbd_info->zone_size_log2)
		zone_idx = offset >> f->zbd_info->zone_size_log2;
	else
		zone_idx = (offset >> 9) / f->zbd_info->zone_size;

	return min(zone_idx, f->zbd_info->nr_zones);
}

/**
 * zbd_zone_full - verify whether a minimum number of bytes remain in a zone
 * @f: file pointer.
 * @z: zone info pointer.
 * @required: minimum number of bytes that must remain in a zone.
 *
 * The caller must hold z->mutex.
 */
static bool zbd_zone_full(const struct fio_file *f, struct fio_zone_info *z,
			  uint64_t required)
{
	assert((required & 511) == 0);

	return z->type == BLK_ZONE_TYPE_SEQWRITE_REQ &&
		z->wp + (required >> 9) > z->start + f->zbd_info->zone_size;
}

static bool is_valid_offset(const struct fio_file *f, uint64_t offset)
{
	return (uint64_t)(offset - f->file_offset) < f->io_size;
}

/* Verify whether direct I/O is used for all host-managed zoned drives. */
static bool zbd_using_direct_io(void)
{
	struct thread_data *td;
	struct fio_file *f;
	int i, j;

	for_each_td(td, i) {
		if (td->o.odirect || !(td->o.td_ddir & TD_DDIR_WRITE))
			continue;
		for_each_file(td, f, j) {
			if (f->zbd_info &&
			    f->zbd_info->model == ZBD_DM_HOST_MANAGED)
				return false;
		}
	}

	return true;
}

/* Whether or not the I/O range for f includes one or more sequential zones */
static bool zbd_is_seq_job(struct fio_file *f)
{
	uint32_t zone_idx, zone_idx_b, zone_idx_e;

	assert(f->zbd_info);
	if (f->io_size == 0)
		return false;
	zone_idx_b = zbd_zone_idx(f, f->file_offset);
	zone_idx_e = zbd_zone_idx(f, f->file_offset + f->io_size - 1);
	for (zone_idx = zone_idx_b; zone_idx <= zone_idx_e; zone_idx++)
		if (f->zbd_info->zone_info[zone_idx].type ==
		    BLK_ZONE_TYPE_SEQWRITE_REQ)
			return true;

	return false;
}

/*
 * Verify whether offset and size parameters are aligned with zone boundaries.
 */
static bool zbd_verify_sizes(void)
{
	const struct fio_zone_info *z;
	struct thread_data *td;
	struct fio_file *f;
	uint64_t new_offset, new_end;
	uint32_t zone_idx;
	int i, j;

	for_each_td(td, i) {
		for_each_file(td, f, j) {
			if (!f->zbd_info)
				continue;
			if (f->file_offset >= f->real_file_size)
				continue;
			if (!zbd_is_seq_job(f))
				continue;
			zone_idx = zbd_zone_idx(f, f->file_offset);
			z = &f->zbd_info->zone_info[zone_idx];
			if (f->file_offset != (z->start << 9)) {
				new_offset = (z+1)->start << 9;
				if (new_offset >= f->file_offset + f->io_size) {
					log_info("%s: io_size must be at least one zone\n",
						 f->file_name);
					return false;
				}
				log_info("%s: rounded up offset from %lu to %lu\n",
					 f->file_name, f->file_offset,
					 new_offset);
				f->io_size -= (new_offset - f->file_offset);
				f->file_offset = new_offset;
			}
			zone_idx = zbd_zone_idx(f, f->file_offset + f->io_size);
			z = &f->zbd_info->zone_info[zone_idx];
			new_end = z->start << 9;
			if (f->file_offset + f->io_size != new_end) {
				if (new_end <= f->file_offset) {
					log_info("%s: io_size must be at least one zone\n",
						 f->file_name);
					return false;
				}
				log_info("%s: rounded down io_size from %lu to %lu\n",
					 f->file_name, f->io_size,
					 new_end - f->file_offset);
				f->io_size = new_end - f->file_offset;
			}
		}
	}

	return true;
}

static bool zbd_verify_bs(void)
{
	struct thread_data *td;
	struct fio_file *f;
	uint32_t zone_size;
	int i, j, k;

	for_each_td(td, i) {
		for_each_file(td, f, j) {
			if (!f->zbd_info)
				continue;
			zone_size = f->zbd_info->zone_size;
			for (k = 0; k < ARRAY_SIZE(td->o.bs); k++) {
				if (td->o.verify != VERIFY_NONE &&
				    (zone_size << 9) % td->o.bs[k] != 0) {
					log_info("%s: block size %llu is not a divisor of the zone size %d\n",
						 f->file_name, td->o.bs[k],
						 zone_size << 9);
					return false;
				}
			}
		}
	}
	return true;
}

/*
 * Read zone information into @buf starting from sector @start_sector.
 * @fd is a file descriptor that refers to a block device and @bufsz is the
 * size of @buf.
 *
 * Returns 0 upon success and a negative error code upon failure.
 */
static int read_zone_info(int fd, uint64_t start_sector,
			  void *buf, unsigned int bufsz)
{
	struct blk_zone_report *hdr = buf;

	if (bufsz < sizeof(*hdr))
		return -EINVAL;

	memset(hdr, 0, sizeof(*hdr));

	hdr->nr_zones = (bufsz - sizeof(*hdr)) / sizeof(struct blk_zone);
	hdr->sector = start_sector;
	return ioctl(fd, BLKREPORTZONE, hdr) >= 0 ? 0 : -errno;
}

/*
 * Read up to 255 characters from the first line of a file. Strip the trailing
 * newline.
 */
static char *read_file(const char *path)
{
	char line[256], *p = line;
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (!fgets(line, sizeof(line), f))
		line[0] = '\0';
	strsep(&p, "\n");
	fclose(f);

	return strdup(line);
}

static enum blk_zoned_model get_zbd_model(const char *file_name)
{
	enum blk_zoned_model model = ZBD_DM_NONE;
	char *zoned_attr_path = NULL;
	char *model_str = NULL;
	struct stat statbuf;

	if (stat(file_name, &statbuf) < 0)
		goto out;
	if (asprintf(&zoned_attr_path, "/sys/dev/block/%d:%d/queue/zoned",
		     major(statbuf.st_rdev), minor(statbuf.st_rdev)) < 0)
		goto out;
	model_str = read_file(zoned_attr_path);
	if (!model_str)
		goto out;
	dprint(FD_ZBD, "%s: zbd model string: %s\n", file_name, model_str);
	if (strcmp(model_str, "host-aware") == 0)
		model = ZBD_DM_HOST_AWARE;
	else if (strcmp(model_str, "host-managed") == 0)
		model = ZBD_DM_HOST_MANAGED;

out:
	free(model_str);
	free(zoned_attr_path);
	return model;
}

static int ilog2(uint64_t i)
{
	int log = -1;

	while (i) {
		i >>= 1;
		log++;
	}
	return log;
}

/*
 * Initialize f->zbd_info for devices that are not zoned block devices. This
 * allows to execute a ZBD workload against a non-ZBD device.
 */
static int init_zone_info(struct thread_data *td, struct fio_file *f)
{
	uint32_t nr_zones;
	struct fio_zone_info *p;
	uint64_t zone_size;
	struct zoned_block_device_info *zbd_info = NULL;
	pthread_mutexattr_t attr;
	int i;

	zone_size = td->o.zone_size >> 9;
	assert(zone_size);
	nr_zones = ((f->real_file_size >> 9) + zone_size - 1) / zone_size;
	zbd_info = scalloc(1, sizeof(*zbd_info) +
			   (nr_zones + 1) * sizeof(zbd_info->zone_info[0]));
	if (!zbd_info)
		return -ENOMEM;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutexattr_setpshared(&attr, true);
	pthread_mutex_init(&zbd_info->mutex, &attr);
	zbd_info->refcount = 1;
	p = &zbd_info->zone_info[0];
	for (i = 0; i < nr_zones; i++, p++) {
		pthread_mutex_init(&p->mutex, &attr);
		p->start = i * zone_size;
		p->wp = p->start + zone_size;
		p->type = BLK_ZONE_TYPE_SEQWRITE_REQ;
		p->cond = BLK_ZONE_COND_EMPTY;
	}
	/* a sentinel */
	p->start = nr_zones * zone_size;

	f->zbd_info = zbd_info;
	f->zbd_info->zone_size = zone_size;
	f->zbd_info->zone_size_log2 = is_power_of_2(zone_size) ?
		ilog2(zone_size) + 9 : -1;
	f->zbd_info->nr_zones = nr_zones;
	pthread_mutexattr_destroy(&attr);
	return 0;
}

/*
 * Parse the BLKREPORTZONE output and store it in f->zbd_info. Must be called
 * only for devices that support this ioctl, namely zoned block devices.
 */
static int parse_zone_info(struct thread_data *td, struct fio_file *f)
{
	const unsigned int bufsz = sizeof(struct blk_zone_report) +
		4096 * sizeof(struct blk_zone);
	uint32_t nr_zones;
	struct blk_zone_report *hdr;
	const struct blk_zone *z;
	struct fio_zone_info *p;
	uint64_t zone_size, start_sector;
	struct zoned_block_device_info *zbd_info = NULL;
	pthread_mutexattr_t attr;
	void *buf;
	int fd, i, j, ret = 0;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutexattr_setpshared(&attr, true);

	buf = malloc(bufsz);
	if (!buf)
		goto out;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0) {
		ret = -errno;
		goto free;
	}

	ret = read_zone_info(fd, 0, buf, bufsz);
	if (ret < 0) {
		log_info("fio: BLKREPORTZONE(%lu) failed for %s (%d).\n",
			 0UL, f->file_name, -ret);
		goto close;
	}
	hdr = buf;
	if (hdr->nr_zones < 1) {
		log_info("fio: %s has invalid zone information.\n",
			 f->file_name);
		goto close;
	}
	z = (void *)(hdr + 1);
	zone_size = z->len;
	nr_zones = ((f->real_file_size >> 9) + zone_size - 1) / zone_size;

	if (td->o.zone_size == 0) {
		td->o.zone_size = zone_size << 9;
	} else if (td->o.zone_size != zone_size << 9) {
		log_info("fio: %s job parameter zonesize %lld does not match disk zone size %ld.\n",
			 f->file_name, td->o.zone_size, zone_size << 9);
		ret = -EINVAL;
		goto close;
	}

	dprint(FD_ZBD, "Device %s has %d zones of size %lu KB\n", f->file_name,
	       nr_zones, zone_size / 2);

	zbd_info = scalloc(1, sizeof(*zbd_info) +
			   (nr_zones + 1) * sizeof(zbd_info->zone_info[0]));
	ret = -ENOMEM;
	if (!zbd_info)
		goto close;
	pthread_mutex_init(&zbd_info->mutex, &attr);
	zbd_info->refcount = 1;
	p = &zbd_info->zone_info[0];
	for (start_sector = 0, j = 0; j < nr_zones;) {
		z = (void *)(hdr + 1);
		for (i = 0; i < hdr->nr_zones; i++, j++, z++, p++) {
			pthread_mutex_init(&p->mutex, &attr);
			p->start = z->start;
			switch (z->cond) {
			case BLK_ZONE_COND_NOT_WP:
				p->wp = z->start;
				break;
			case BLK_ZONE_COND_FULL:
				p->wp = z->start + zone_size;
				break;
			default:
				assert(z->start <= z->wp);
				assert(z->wp <= z->start + zone_size);
				p->wp = z->wp;
				break;
			}
			p->type = z->type;
			p->cond = z->cond;
			if (j > 0 && p->start != p[-1].start + zone_size) {
				log_info("%s: invalid zone data\n",
					 f->file_name);
				ret = -EINVAL;
				goto close;
			}
		}
		z--;
		start_sector = z->start + z->len;
		if (j >= nr_zones)
			break;
		ret = read_zone_info(fd, start_sector, buf, bufsz);
		if (ret < 0) {
			log_info("fio: BLKREPORTZONE(%lu) failed for %s (%d).\n",
				 start_sector, f->file_name, -ret);
			goto close;
		}
	}
	/* a sentinel */
	zbd_info->zone_info[nr_zones].start = start_sector;

	f->zbd_info = zbd_info;
	f->zbd_info->zone_size = zone_size;
	f->zbd_info->zone_size_log2 = is_power_of_2(zone_size) ?
		ilog2(zone_size) + 9 : -1;
	f->zbd_info->nr_zones = nr_zones;
	zbd_info = NULL;
	ret = 0;

close:
	sfree(zbd_info);
	close(fd);
free:
	free(buf);
out:
	pthread_mutexattr_destroy(&attr);
	return ret;
}

/*
 * Allocate zone information and store it into f->zbd_info if zonemode=zbd.
 *
 * Returns 0 upon success and a negative error code upon failure.
 */
int zbd_create_zone_info(struct thread_data *td, struct fio_file *f)
{
	enum blk_zoned_model zbd_model;
	int ret = 0;

	assert(td->o.zone_mode == ZONE_MODE_ZBD);

	zbd_model = get_zbd_model(f->file_name);
	switch (zbd_model) {
	case ZBD_DM_HOST_AWARE:
	case ZBD_DM_HOST_MANAGED:
		ret = parse_zone_info(td, f);
		break;
	case ZBD_DM_NONE:
		ret = init_zone_info(td, f);
		break;
	}
	if (ret == 0)
		f->zbd_info->model = zbd_model;
	return ret;
}

void zbd_free_zone_info(struct fio_file *f)
{
	uint32_t refcount;

	if (!f->zbd_info)
		return;

	pthread_mutex_lock(&f->zbd_info->mutex);
	refcount = --f->zbd_info->refcount;
	pthread_mutex_unlock(&f->zbd_info->mutex);

	assert((int32_t)refcount >= 0);
	if (refcount == 0)
		sfree(f->zbd_info);
	f->zbd_info = NULL;
}

/*
 * Initialize f->zbd_info.
 *
 * Returns 0 upon success and a negative error code upon failure.
 *
 * Note: this function can only work correctly if it is called before the first
 * fio fork() call.
 */
static int zbd_init_zone_info(struct thread_data *td, struct fio_file *file)
{
	struct thread_data *td2;
	struct fio_file *f2;
	int i, j, ret;

	for_each_td(td2, i) {
		for_each_file(td2, f2, j) {
			if (td2 == td && f2 == file)
				continue;
			if (!f2->zbd_info ||
			    strcmp(f2->file_name, file->file_name) != 0)
				continue;
			file->zbd_info = f2->zbd_info;
			file->zbd_info->refcount++;
			return 0;
		}
	}

	ret = zbd_create_zone_info(td, file);
	if (ret < 0)
		td_verror(td, -ret, "BLKREPORTZONE failed");
	return ret;
}

int zbd_init(struct thread_data *td)
{
	struct fio_file *f;
	int i;

	for_each_file(td, f, i) {
		if (f->filetype != FIO_TYPE_BLOCK)
			continue;
		if (td->o.zone_size && td->o.zone_size < 512) {
			log_err("%s: zone size must be at least 512 bytes for --zonemode=zbd\n\n",
				f->file_name);
			return 1;
		}
		if (td->o.zone_size == 0 &&
		    get_zbd_model(f->file_name) == ZBD_DM_NONE) {
			log_err("%s: Specifying the zone size is mandatory for regular block devices with --zonemode=zbd\n\n",
				f->file_name);
			return 1;
		}
		zbd_init_zone_info(td, f);
	}

	if (!zbd_using_direct_io()) {
		log_err("Using direct I/O is mandatory for writing to ZBD drives\n\n");
		return 1;
	}

	if (!zbd_verify_sizes())
		return 1;

	if (!zbd_verify_bs())
		return 1;

	return 0;
}

/**
 * zbd_reset_range - reset zones for a range of sectors
 * @td: FIO thread data.
 * @f: Fio file for which to reset zones
 * @sector: Starting sector in units of 512 bytes
 * @nr_sectors: Number of sectors in units of 512 bytes
 *
 * Returns 0 upon success and a negative error code upon failure.
 */
static int zbd_reset_range(struct thread_data *td, const struct fio_file *f,
			   uint64_t sector, uint64_t nr_sectors)
{
	struct blk_zone_range zr = {
		.sector         = sector,
		.nr_sectors     = nr_sectors,
	};
	uint32_t zone_idx_b, zone_idx_e;
	struct fio_zone_info *zb, *ze, *z;
	int ret = 0;

	assert(f->fd != -1);
	assert(is_valid_offset(f, ((sector + nr_sectors) << 9) - 1));
	switch (f->zbd_info->model) {
	case ZBD_DM_HOST_AWARE:
	case ZBD_DM_HOST_MANAGED:
		ret = ioctl(f->fd, BLKRESETZONE, &zr);
		if (ret < 0) {
			td_verror(td, errno, "resetting wp failed");
			log_err("%s: resetting wp for %llu sectors at sector %llu failed (%d).\n",
				f->file_name, zr.nr_sectors, zr.sector, errno);
			return ret;
		}
		break;
	case ZBD_DM_NONE:
		break;
	}

	zone_idx_b = zbd_zone_idx(f, sector << 9);
	zb = &f->zbd_info->zone_info[zone_idx_b];
	zone_idx_e = zbd_zone_idx(f, (sector + nr_sectors) << 9);
	ze = &f->zbd_info->zone_info[zone_idx_e];
	for (z = zb; z < ze; z++) {
		pthread_mutex_lock(&z->mutex);
		z->wp = z->start;
		z->verify_block = 0;
		pthread_mutex_unlock(&z->mutex);
	}

	return ret;
}

/**
 * zbd_reset_zone - reset the write pointer of a single zone
 * @td: FIO thread data.
 * @f: FIO file associated with the disk for which to reset a write pointer.
 * @z: Zone to reset.
 *
 * Returns 0 upon success and a negative error code upon failure.
 */
static int zbd_reset_zone(struct thread_data *td, const struct fio_file *f,
			  struct fio_zone_info *z)
{
	int ret;

	dprint(FD_ZBD, "%s: resetting wp of zone %lu.\n", f->file_name,
	       z - f->zbd_info->zone_info);
	ret = zbd_reset_range(td, f, z->start, (z+1)->start - z->start);
	return ret;
}

/*
 * Reset a range of zones. Returns 0 upon success and 1 upon failure.
 * @td: fio thread data.
 * @f: fio file for which to reset zones
 * @zb: first zone to reset.
 * @ze: first zone not to reset.
 * @all_zones: whether to reset all zones or only those zones for which the
 *	write pointer is not a multiple of td->o.min_bs[DDIR_WRITE].
 */
static int zbd_reset_zones(struct thread_data *td, struct fio_file *f,
			   struct fio_zone_info *const zb,
			   struct fio_zone_info *const ze, bool all_zones)
{
	struct fio_zone_info *z, *start_z = ze;
	const uint32_t min_bs = td->o.min_bs[DDIR_WRITE] >> 9;
	bool reset_wp;
	int res = 0;

	dprint(FD_ZBD, "%s: examining zones %lu .. %lu\n", f->file_name,
	       zb - f->zbd_info->zone_info, ze - f->zbd_info->zone_info);
	assert(f->fd != -1);
	for (z = zb; z < ze; z++) {
		pthread_mutex_lock(&z->mutex);
		switch (z->type) {
		case BLK_ZONE_TYPE_SEQWRITE_REQ:
			reset_wp = all_zones ? z->wp != z->start :
					(td->o.td_ddir & TD_DDIR_WRITE) &&
					z->wp % min_bs != 0;
			if (start_z == ze && reset_wp) {
				start_z = z;
			} else if (start_z < ze && !reset_wp) {
				dprint(FD_ZBD,
				       "%s: resetting zones %lu .. %lu\n",
				       f->file_name,
				       start_z - f->zbd_info->zone_info,
				       z - f->zbd_info->zone_info);
				if (zbd_reset_range(td, f, start_z->start,
						z->start - start_z->start) < 0)
					res = 1;
				start_z = ze;
			}
			break;
		default:
			if (start_z == ze)
				break;
			dprint(FD_ZBD, "%s: resetting zones %lu .. %lu\n",
			       f->file_name, start_z - f->zbd_info->zone_info,
			       z - f->zbd_info->zone_info);
			if (zbd_reset_range(td, f, start_z->start,
					    z->start - start_z->start) < 0)
				res = 1;
			start_z = ze;
			break;
		}
	}
	if (start_z < ze) {
		dprint(FD_ZBD, "%s: resetting zones %lu .. %lu\n", f->file_name,
		       start_z - f->zbd_info->zone_info,
		       z - f->zbd_info->zone_info);
		if (zbd_reset_range(td, f, start_z->start,
				    z->start - start_z->start) < 0)
			res = 1;
	}
	for (z = zb; z < ze; z++)
		pthread_mutex_unlock(&z->mutex);

	return res;
}

void zbd_file_reset(struct thread_data *td, struct fio_file *f)
{
	struct fio_zone_info *zb, *ze;
	uint32_t zone_idx_e;

	if (!f->zbd_info)
		return;

	zb = &f->zbd_info->zone_info[zbd_zone_idx(f, f->file_offset)];
	zone_idx_e = zbd_zone_idx(f, f->file_offset + f->io_size);
	ze = &f->zbd_info->zone_info[zone_idx_e];
	/*
	 * If data verification is enabled reset the affected zones before
	 * writing any data to avoid that a zone reset has to be issued while
	 * writing data, which causes data loss.
	 */
	zbd_reset_zones(td, f, zb, ze, td->o.verify != VERIFY_NONE &&
			(td->o.td_ddir & TD_DDIR_WRITE) &&
			td->runstate != TD_VERIFYING);
}

/* The caller must hold z->mutex. */
static void zbd_replay_write_order(struct thread_data *td, struct io_u *io_u,
				   struct fio_zone_info *z)
{
	const struct fio_file *f = io_u->file;
	const uint32_t min_bs = td->o.min_bs[DDIR_WRITE];

	if (z->verify_block * min_bs >= f->zbd_info->zone_size)
		log_err("%s: %d * %d >= %ld\n", f->file_name, z->verify_block,
			min_bs, f->zbd_info->zone_size);
	io_u->offset = (z->start << 9) + z->verify_block++ * min_bs;
}

/*
 * Find another zone for which @io_u fits below the write pointer. Start
 * searching in zones @zb + 1 .. @zl and continue searching in zones
 * @zf .. @zb - 1.
 *
 * Either returns NULL or returns a zone pointer and holds the mutex for that
 * zone.
 */
static struct fio_zone_info *
zbd_find_zone(struct thread_data *td, struct io_u *io_u,
	      struct fio_zone_info *zb, struct fio_zone_info *zl)
{
	const uint32_t min_bs = td->o.min_bs[io_u->ddir];
	const struct fio_file *f = io_u->file;
	struct fio_zone_info *z1, *z2;
	const struct fio_zone_info *const zf =
		&f->zbd_info->zone_info[zbd_zone_idx(f, f->file_offset)];

	/*
	 * Skip to the next non-empty zone in case of sequential I/O and to
	 * the nearest non-empty zone in case of random I/O.
	 */
	for (z1 = zb + 1, z2 = zb - 1; z1 < zl || z2 >= zf; z1++, z2--) {
		if (z1 < zl && z1->cond != BLK_ZONE_COND_OFFLINE) {
			pthread_mutex_lock(&z1->mutex);
			if (z1->start + (min_bs >> 9) <= z1->wp)
				return z1;
			pthread_mutex_unlock(&z1->mutex);
		} else if (!td_random(td)) {
			break;
		}
		if (td_random(td) && z2 >= zf &&
		    z2->cond != BLK_ZONE_COND_OFFLINE) {
			pthread_mutex_lock(&z2->mutex);
			if (z2->start + (min_bs >> 9) <= z2->wp)
				return z2;
			pthread_mutex_unlock(&z2->mutex);
		}
	}
	dprint(FD_ZBD, "%s: adjusting random read offset failed\n",
	       f->file_name);
	return NULL;
}


/**
 * zbd_post_submit - update the write pointer and unlock the zone lock
 * @io_u: I/O unit
 * @success: Whether or not the I/O unit has been executed successfully
 *
 * For write and trim operations, update the write pointer of all affected
 * zones.
 */
static void zbd_post_submit(const struct io_u *io_u, bool success)
{
	struct zoned_block_device_info *zbd_info;
	struct fio_zone_info *z;
	uint32_t zone_idx;
	uint64_t end, zone_end;

	zbd_info = io_u->file->zbd_info;
	if (!zbd_info)
		return;

	zone_idx = zbd_zone_idx(io_u->file, io_u->offset);
	end = (io_u->offset + io_u->buflen) >> 9;
	z = &zbd_info->zone_info[zone_idx];
	assert(zone_idx < zbd_info->nr_zones);
	if (z->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return;
	if (!success)
		goto unlock;
	switch (io_u->ddir) {
	case DDIR_WRITE:
		zone_end = min(end, (z + 1)->start);
		z->wp = zone_end;
		break;
	case DDIR_TRIM:
		assert(z->wp == z->start);
		break;
	default:
		break;
	}
unlock:
	pthread_mutex_unlock(&z->mutex);
}

bool zbd_unaligned_write(int error_code)
{
	switch (error_code) {
	case EIO:
	case EREMOTEIO:
		return true;
	}
	return false;
}

/**
 * zbd_adjust_block - adjust the offset and length as necessary for ZBD drives
 * @td: FIO thread data.
 * @io_u: FIO I/O unit.
 *
 * Locking strategy: returns with z->mutex locked if and only if z refers
 * to a sequential zone and if io_u_accept is returned. z is the zone that
 * corresponds to io_u->offset at the end of this function.
 */
enum io_u_action zbd_adjust_block(struct thread_data *td, struct io_u *io_u)
{
	const struct fio_file *f = io_u->file;
	uint32_t zone_idx_b;
	struct fio_zone_info *zb, *zl;
	uint32_t orig_len = io_u->buflen;
	uint32_t min_bs = td->o.min_bs[io_u->ddir];
	uint64_t new_len;
	int64_t range;

	if (!f->zbd_info)
		return io_u_accept;

	assert(is_valid_offset(f, io_u->offset));
	assert(io_u->buflen);
	zone_idx_b = zbd_zone_idx(f, io_u->offset);
	zb = &f->zbd_info->zone_info[zone_idx_b];

	/* Accept the I/O offset for conventional zones. */
	if (zb->type == BLK_ZONE_TYPE_CONVENTIONAL)
		return io_u_accept;

	/*
	 * Accept the I/O offset for reads if reading beyond the write pointer
	 * is enabled.
	 */
	if (zb->cond != BLK_ZONE_COND_OFFLINE &&
	    io_u->ddir == DDIR_READ && td->o.read_beyond_wp)
		return io_u_accept;

	pthread_mutex_lock(&zb->mutex);
	switch (io_u->ddir) {
	case DDIR_READ:
		if (td->runstate == TD_VERIFYING) {
			zbd_replay_write_order(td, io_u, zb);
			goto accept;
		}
		/*
		 * Avoid reads past the write pointer because such reads do not
		 * hit the medium.
		 */
		range = zb->cond != BLK_ZONE_COND_OFFLINE ?
			((zb->wp - zb->start) << 9) - io_u->buflen : 0;
		if (td_random(td) && range >= 0) {
			io_u->offset = (zb->start << 9) +
				((io_u->offset - (zb->start << 9)) %
				 (range + 1)) / min_bs * min_bs;
			assert(zb->start << 9 <= io_u->offset);
			assert(io_u->offset + io_u->buflen <= zb->wp << 9);
			goto accept;
		}
		if (zb->cond == BLK_ZONE_COND_OFFLINE ||
		    (io_u->offset + io_u->buflen) >> 9 > zb->wp) {
			pthread_mutex_unlock(&zb->mutex);
			zl = &f->zbd_info->zone_info[zbd_zone_idx(f,
						f->file_offset + f->io_size)];
			zb = zbd_find_zone(td, io_u, zb, zl);
			if (!zb) {
				dprint(FD_ZBD,
				       "%s: zbd_find_zone(%lld, %llu) failed\n",
				       f->file_name, io_u->offset,
				       io_u->buflen);
				goto eof;
			}
			io_u->offset = zb->start << 9;
		}
		if ((io_u->offset + io_u->buflen) >> 9 > zb->wp) {
			dprint(FD_ZBD, "%s: %lld + %lld > %" PRIu64 "\n",
			       f->file_name, io_u->offset, io_u->buflen,
			       zb->wp);
			goto eof;
		}
		goto accept;
	case DDIR_WRITE:
		if (io_u->buflen > (f->zbd_info->zone_size << 9))
			goto eof;
		/* Reset the zone pointer if necessary */
		if (zb->reset_zone || zbd_zone_full(f, zb, min_bs)) {
			assert(td->o.verify == VERIFY_NONE);
			/*
			 * Since previous write requests may have been submitted
			 * asynchronously and since we will submit the zone
			 * reset synchronously, wait until previously submitted
			 * write requests have completed before issuing a
			 * zone reset.
			 */
			io_u_quiesce(td);
			zb->reset_zone = 0;
			if (zbd_reset_zone(td, f, zb) < 0)
				goto eof;
		}
		/* Make writes occur at the write pointer */
		assert(!zbd_zone_full(f, zb, min_bs));
		io_u->offset = zb->wp << 9;
		if (!is_valid_offset(f, io_u->offset)) {
			dprint(FD_ZBD, "Dropped request with offset %llu\n",
			       io_u->offset);
			goto eof;
		}
		/*
		 * Make sure that the buflen is a multiple of the minimal
		 * block size. Give up if shrinking would make the request too
		 * small.
		 */
		new_len = min((unsigned long long)io_u->buflen,
			      ((zb + 1)->start << 9) - io_u->offset);
		new_len = new_len / min_bs * min_bs;
		if (new_len == io_u->buflen)
			goto accept;
		if (new_len >= min_bs) {
			io_u->buflen = new_len;
			dprint(FD_IO, "Changed length from %u into %llu\n",
			       orig_len, io_u->buflen);
			goto accept;
		}
		log_err("Zone remainder %lld smaller than minimum block size %d\n",
			(((zb + 1)->start << 9) - io_u->offset),
			min_bs);
		goto eof;
	case DDIR_TRIM:
		/* fall-through */
	case DDIR_SYNC:
	case DDIR_DATASYNC:
	case DDIR_SYNC_FILE_RANGE:
	case DDIR_WAIT:
	case DDIR_LAST:
	case DDIR_INVAL:
		goto accept;
	}

	assert(false);

accept:
	assert(zb);
	assert(zb->cond != BLK_ZONE_COND_OFFLINE);
	assert(!io_u->post_submit);
	io_u->post_submit = zbd_post_submit;
	return io_u_accept;

eof:
	if (zb)
		pthread_mutex_unlock(&zb->mutex);
	return io_u_eof;
}
