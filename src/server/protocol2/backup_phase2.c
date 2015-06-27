#include "include.h"
#include "champ_chooser/include.h"
#include "dpth.h"
#include "../manios.h"
#include "../resume.h"
#include "../../attribs.h"
#include "../../base64.h"
#include "../../cmd.h"
#include "../../hexmap.h"
#include "../../server/manio.h"
#include "../../protocol2/blist.h"
#include "../../slist.h"

#define END_SIGS		0x01
#define END_BACKUP		0x02
#define END_REQUESTS		0x04
#define END_BLK_REQUESTS	0x08

static int data_needed(struct sbuf *sb)
{
	if(sb->path.cmd==CMD_FILE) return 1;
	return 0;
}

// Return -1 for error, 0 for entry not changed, 1 for entry changed (or new).
static int found_in_current_manifest(struct sbuf *csb, struct sbuf *sb,
	struct manios *manios, struct blk **blk, struct conf **confs)
{
	// Located the entry in the current manifest.
	// If the file type changed, I think it is time to back it up again
	// (for example, EFS changing to normal file, or back again).
	if(csb->path.cmd!=sb->path.cmd)
	{
		if(manio_forward_through_sigs(csb,
			blk, manios->current, confs)<0)
				return -1;
		return 1;
	}

	// mtime is the actual file data.
	// ctime is the attributes or meta data.
	if(csb->statp.st_mtime==sb->statp.st_mtime
	  && csb->statp.st_ctime==sb->statp.st_ctime)
	{
		// Got an unchanged file.
		if(manio_copy_entry(csb, sb, blk,
			manios->current, manios->unchanged, confs)<0)
				return -1;
		return 0;
	}

	if(csb->statp.st_mtime==sb->statp.st_mtime
	  && csb->statp.st_ctime!=sb->statp.st_ctime)
	{
		// File data stayed the same, but attributes or meta data
		// changed. We already have the attributes, but may need to
		// get extra meta data.
		// FIX THIS
		if(manio_copy_entry(csb, sb, blk,
			manios->current, manios->unchanged, confs)<0)
				return -1;
		return 0;
	}

	// File data changed.
	if(manio_forward_through_sigs(csb, blk, manios->current, confs)<0)
		return -1;
	return 1;
}

// Return -1 for error, 0 for entry not changed, 1 for entry changed (or new).
static int entry_changed(struct sbuf *sb,
	struct manios *manios, struct conf **confs)
{
	static int finished=0;
	static struct sbuf *csb=NULL;
	static struct blk *blk=NULL;

	if(finished) return 1;

	if(!csb && !(csb=sbuf_alloc(confs))) return -1;

	if(csb->path.buf)
	{
		// Already have an entry.
	}
	else
	{
		// Need to read another.
		if(!blk && !(blk=blk_alloc())) return -1;
		switch(manio_read_with_blk(manios->current,
			csb, blk, NULL, confs))
		{
			case 1: // Reached the end.
				sbuf_free(&csb);
				blk_free(&blk);
				finished=1;
				return 1;
			case -1: return -1;
		}
		if(!csb->path.buf)
		{
			logp("Should have a path at this point, but do not, in %s\n", __func__);
			return -1;
		}
		// Got an entry.
	}

	while(1)
	{
		switch(sbuf_pathcmp(csb, sb))
		{
			case 0: return found_in_current_manifest(csb, sb,
					manios, &blk, confs);
			case 1: return 1;
			case -1:
				// Behind - need to read more data from the old
				// manifest.
				switch(manio_read_with_blk(manios->current,
					csb, blk, NULL, confs))
				{
					case 1: // Reached the end.
						sbuf_free(&csb);
						blk_free(&blk);
						return 1;
					case -1: return -1;
				}
				// Got something, go back around the loop.
		}
	}

	return 0;
}

static int add_data_to_store(struct conf **confs,
	struct slist *slist, struct iobuf *rbuf, struct dpth *dpth)
{
	static struct blk *blk=NULL;
	struct cntr *cntr=get_cntr(confs);

	// Find the first one in the list that was requested.
	// FIX THIS: Going up the list here, and then later
	// when writing to the manifest is not efficient.
	for(blk=slist->blist->head;
		blk && (!blk->requested || blk->got==BLK_GOT); blk=blk->next)
	{
	//	logp("try: %d %d\n", blk->index, blk->got);
	}
	if(!blk)
	{
		logp("Received data but could not find next requested block.\n");
		if(!slist->blist->head)
			logp("and slist->blist->head is null\n");
		else
			logp("head index: %d\n", slist->blist->head->index);
		return -1;
	}

	// Add it to the data store straight away.
	if(dpth_protocol2_fwrite(dpth, rbuf, blk)) return -1;

	cntr_add(cntr, CMD_DATA, 0);
	cntr_add_recvbytes(cntr, blk->length);

	blk->got=BLK_GOT;
	blk=blk->next;

	return 0;
}

static int set_up_for_sig_info(struct slist *slist, struct sbuf *inew)
{
	struct sbuf *sb;

	for(sb=slist->add_sigs_here; sb; sb=sb->next)
	{
		if(!sb->protocol2->index) continue;
		if(inew->protocol2->index==sb->protocol2->index) break;
	}
	if(!sb)
	{
		logp("Could not find %lu in request list %d\n",
			inew->protocol2->index, sb->protocol2->index);
		return -1;
	}
	// Replace the attribs with the more recent values.
	iobuf_free_content(&sb->attr);
	iobuf_move(&sb->attr, &inew->attr);

	// Mark the end of the previous file.
	slist->add_sigs_here->protocol2->bend=slist->blist->tail;

	slist->add_sigs_here=sb;

	// Incoming sigs now need to get added to 'add_sigs_here'
	return 0;
}

/*
static void dump_blks(const char *msg, struct blk *b)
{
	struct blk *xx;
	for(xx=b; xx; xx=xx->next)
		printf("%s: %d %d %p\n", msg, xx->index, xx->got, xx);
}
*/

static int add_to_sig_list(struct slist *slist,
	struct iobuf *rbuf, struct conf **confs)
{
	// Goes on slist->add_sigs_here
	struct blk *blk;
	struct protocol2 *protocol2;

	if(!(blk=blk_alloc())) return -1;
	blist_add_blk(slist->blist, blk);

	protocol2=slist->add_sigs_here->protocol2;
        if(!protocol2->bstart) protocol2->bstart=blk;
        if(!protocol2->bsighead) protocol2->bsighead=blk;

	if(split_sig(rbuf, blk)) return -1;

	// Need to send sigs to champ chooser, therefore need to point
	// to the oldest unsent one if nothing is pointed to yet.
	if(!slist->blist->blk_for_champ_chooser)
		slist->blist->blk_for_champ_chooser=blk;

	return 0;
}

static int deal_with_read(struct iobuf *rbuf, struct slist *slist,
	struct conf **confs, uint8_t *end_flags, struct dpth *dpth)
{
	int ret=0;
	static struct sbuf *inew=NULL;

	if(!inew && !(inew=sbuf_alloc(confs))) goto error;

	switch(rbuf->cmd)
	{
		/* Incoming block data. */
		case CMD_DATA:
			if(add_data_to_store(confs, slist, rbuf, dpth))
				goto error;
			goto end;

		/* Incoming block signatures. */
		case CMD_ATTRIBS_SIGS:
			// New set of stuff incoming. Clean up.
			if(inew->attr.buf) free(inew->attr.buf);
			iobuf_move(&inew->attr, rbuf);
			inew->protocol2->index=decode_file_no(&inew->attr);

			// Need to go through slist to find the matching
			// entry.
			if(set_up_for_sig_info(slist, inew))
				goto error;
			return 0;
		case CMD_SIG:
			if(add_to_sig_list(slist, rbuf, confs))
				goto error;
			goto end;

		/* Incoming control/message stuff. */
		case CMD_MESSAGE:
		case CMD_WARNING:
			log_recvd(rbuf, confs, 0);
			goto end;
		case CMD_GEN:
			if(!strcmp(rbuf->buf, "sigs_end"))
			{
				(*end_flags)|=END_SIGS;
				goto end;
			}
			else if(!strcmp(rbuf->buf, "backup_end"))
			{
				(*end_flags)|=END_BACKUP;
				goto end;
			}
			break;
		default:
			break;
	}

	iobuf_log_unexpected(rbuf, __func__);
error:
	ret=-1;
	sbuf_free(&inew);
end:
	iobuf_free_content(rbuf);
	return ret;
}

static int encode_req(struct blk *blk, char *req)
{
	char *p=req;
	p+=to_base64(blk->index, p);
	*p=0;
	return 0;
}

static int get_wbuf_from_sigs(struct iobuf *wbuf, struct slist *slist,
	uint8_t *end_flags, struct conf **confs)
{
	static char req[32]="";
	struct sbuf *sb=slist->blks_to_request;

	while(sb && !(sb->flags & SBUF_NEED_DATA)) sb=sb->next;

	if(!sb)
	{
		slist->blks_to_request=NULL;
		if((*end_flags)&END_SIGS && !((*end_flags)&END_BLK_REQUESTS))
		{
			iobuf_from_str(wbuf,
				CMD_GEN, (char *)"blk_requests_end");
			(*end_flags)|=END_BLK_REQUESTS;
		}
		return 0;
	}
	if(!sb->protocol2->bsighead)
	{
		// Trying to move onto the next file.
		// ??? Does this really work?
		if(sb->protocol2->bend)
		{
			slist->blks_to_request=sb->next;
			printf("move to next\n");
		}
		if((*end_flags)&END_SIGS && !((*end_flags)&END_BLK_REQUESTS))
		{
			iobuf_from_str(wbuf,
				CMD_GEN, (char *)"blk_requests_end");
			(*end_flags)|=END_BLK_REQUESTS;
		}
		return 0;
	}

	if(sb->protocol2->bsighead->got==BLK_INCOMING)
		return 0;

	if(sb->protocol2->bsighead->got==BLK_NOT_GOT)
	{
		encode_req(sb->protocol2->bsighead, req);
		iobuf_from_str(wbuf, CMD_DATA_REQ, req);
		sb->protocol2->bsighead->requested=1;
	}

	// Move on.
	if(sb->protocol2->bsighead==sb->protocol2->bend)
	{
		slist->blks_to_request=sb->next;
		sb->protocol2->bsighead=sb->protocol2->bstart;
	}
	else
	{
		sb->protocol2->bsighead=sb->protocol2->bsighead->next;
	}
	return 0;
}

static void get_wbuf_from_files(struct iobuf *wbuf, struct slist *slist,
	struct manios *manios, uint8_t *end_flags)
{
	static uint64_t file_no=1;
	struct sbuf *sb=slist->last_requested;
	if(!sb)
	{
		if(!manios->phase1 && !((*end_flags)&END_REQUESTS))
		{
			iobuf_from_str(wbuf, CMD_GEN, (char *)"requests_end");
			(*end_flags)|=END_REQUESTS;
		}
		return;
	}

	if(sb->flags & SBUF_SENT_PATH || !(sb->flags & SBUF_NEED_DATA))
	{
		slist->last_requested=sb->next;
		return;
	}

	// Only need to request the path at this stage.
	iobuf_copy(wbuf, &sb->path);
	sb->flags |= SBUF_SENT_PATH;
	sb->protocol2->index=file_no++;
}

static void sanity_before_sbuf_free(struct slist *slist, struct sbuf *sb)
{
	// It is possible for the markers to drop behind.
	if(slist->tail==sb) slist->tail=sb->next;
	if(slist->last_requested==sb) slist->last_requested=sb->next;
	if(slist->add_sigs_here==sb) slist->add_sigs_here=sb->next;
	if(slist->blks_to_request==sb) slist->blks_to_request=sb->next;
}

static void get_wbuf_from_index(struct iobuf *wbuf, uint64_t index)
{
	static char *p;
	static char tmp[32];
//printf("%s: %d\n", __func__, index);
	p=tmp;
	p+=to_base64(index, tmp);
	*p='\0';
	iobuf_from_str(wbuf, CMD_WRAP_UP, tmp);
}

static int sbuf_needs_data(struct sbuf *sb, struct asfd *asfd,
        struct asfd *chfd, struct manios *manios,
        struct slist *slist, int end_flags, struct conf **confs)
{
	struct blk *blk;
	static struct iobuf *wbuf=NULL;
	struct blist *blist=slist->blist;

	if(!(sb->flags & SBUF_HEADER_WRITTEN_TO_MANIFEST))
	{
		if(manio_write_sbuf(manios->changed, sb)) goto error;
		sb->flags |= SBUF_HEADER_WRITTEN_TO_MANIFEST;
	}

        if(!wbuf && !(wbuf=iobuf_alloc())) return -1;

	while((blk=sb->protocol2->bstart)
		&& blk->got==BLK_GOT
		&& (blk->next || end_flags&END_BACKUP))
	{
		if(blk->got_save_path
		  && !blk_is_zero_length(blk))
		{
			if(manio_write_sig_and_path(manios->changed, blk))
				goto error;
			if(manios->changed->sig_count==0)
			{
				// Have finished a manifest file. Want to start
				// using it as a dedup candidate now.
				iobuf_from_str(wbuf, CMD_MANIFEST,
					manios->changed->offset->fpath);
				if(chfd->write(chfd, wbuf)) goto error;

				if(!blk->requested)
				{
					// Also let the client know, so that it
					// can free memory if there was a long
					// consecutive number of unrequested
					// blocks.
					get_wbuf_from_index(wbuf, blk->index);
					if(asfd->write(asfd, wbuf)) goto error;
				}
			}
		}

		if(blk==sb->protocol2->bend)
		{
			struct iobuf endfile;
			slist->head=sb->next;
			if(!(blist->head=sb->protocol2->bstart))
				blist->tail=NULL;
			sanity_before_sbuf_free(slist, sb);
			sbuf_free(&sb);
			// FIX THIS: Should give a proper length and md5sum.
			// Should probably be part of sb above.
			iobuf_from_str(&endfile, CMD_END_FILE, (char *)"0:0");
			iobuf_send_msg_fzp(&endfile, manios->changed->fzp);
			return 1;
		}

		if(sb->protocol2->bsighead==sb->protocol2->bstart)
			sb->protocol2->bsighead=blk->next;
		sb->protocol2->bstart=blk->next;
		if(blk==blist->blk_from_champ_chooser)
			blist->blk_from_champ_chooser=blk->next;

		//printf("freeing blk %d\n", blk->index);
		blk_free(&blk);
	}

	if(!(blist->head=sb->protocol2->bstart)) blist->tail=NULL;
	return 0;
error:
	return -1;
}

static int write_to_changed_file(struct asfd *asfd,
	struct asfd *chfd, struct manios *manios,
	struct slist *slist, int end_flags, struct conf **confs)
{
	struct sbuf *sb;
	if(!slist) return 0;

	while((sb=slist->head))
	{
		if(sb->flags & SBUF_NEED_DATA)
		{
			switch(sbuf_needs_data(sb, asfd, chfd, manios, slist,
				end_flags, confs))
			{
				case 0: return 0;
				case 1: continue;
				default: return -1;
			}

		}
		else
		{
			// No change, can go straight in.
			if(manio_write_sbuf(manios->changed, sb)) return -1;

			// Move along.
			slist->head=sb->next;

			sanity_before_sbuf_free(slist, sb);
			sbuf_free(&sb);
		}
	}
	return 0;
}

static int maybe_add_from_scan(struct manios *manios,
	struct slist *slist, struct conf **confs)
{
	int ret=-1;
	static int ec=0;
	struct sbuf *snew=NULL;

	while(1)
	{
		if(!manios->phase1) return 0;
		// Limit the amount loaded into memory at any one time.
		if(slist && slist->head)
		{
			if(slist->head->protocol2->index
			  - slist->tail->protocol2->index>4096)
				return 0;
		}
		if(!(snew=sbuf_alloc(confs))) goto end;

		switch(manio_read(manios->phase1, snew, confs))
		{
			case 0: break;
			case 1: manio_close(&manios->phase1);
				return 0; // Finished.
			default: goto end;
		}

		if(!(ec=entry_changed(snew, manios, confs)))
		{
			// No change, no need to add to slist.
			continue;
		}
		else if(ec<0) goto end; // Error.

		if(data_needed(snew)) snew->flags|=SBUF_NEED_DATA;

		slist_add_sbuf(slist, snew);
	}
	return 0;
end:
	sbuf_free(&snew);
	return ret;
}

static int append_for_champ_chooser(struct asfd *chfd,
	struct blist *blist, int end_flags)
{
	static int finished_sending=0;
	static struct iobuf *wbuf=NULL;
	if(!wbuf)
	{
		if(!(wbuf=iobuf_alloc())
		  || !(wbuf->buf=(char *)malloc_w(CHECKSUM_LEN, __func__)))
			return -1;
		wbuf->cmd=CMD_SIG;
	}
	while(blist->blk_for_champ_chooser)
	{
		// If we send too many blocks to the champ chooser at once,
		// it can go faster than we can send paths to completed
		// manifests to it. This means that deduplication efficiency
		// is reduced (although speed may be faster).
		// So limit the sending.
		if(blist->blk_for_champ_chooser->index
		  - blist->head->index > MANIFEST_SIG_MAX)
			return 0;

		// FIX THIS: Maybe convert depending on endian-ness.
		memcpy(wbuf->buf, 
			&blist->blk_for_champ_chooser->fingerprint,
			FINGERPRINT_LEN);
		memcpy(wbuf->buf+FINGERPRINT_LEN,
			blist->blk_for_champ_chooser->md5sum,
			MD5_DIGEST_LENGTH);
		wbuf->len=CHECKSUM_LEN;

		switch(chfd->append_all_to_write_buffer(chfd, wbuf))
		{
			case APPEND_OK: break;
			case APPEND_BLOCKED:
				return 0; // Try again later.
			default: return -1;
		}
		blist->blk_for_champ_chooser=blist->blk_for_champ_chooser->next;
	}
	if(end_flags&END_SIGS
	  && !finished_sending && !blist->blk_for_champ_chooser)
	{
		wbuf->cmd=CMD_GEN;
		wbuf->len=snprintf(wbuf->buf, CHECKSUM_LEN, "%s", "sigs_end");
		switch(chfd->append_all_to_write_buffer(chfd, wbuf))
		{
			case APPEND_OK: break;
			case APPEND_BLOCKED:
				return 0; // Try again later.
			default: return -1;
		}
		finished_sending++;
	}
	return 0;
}

static int mark_not_got(struct blk *blk, struct dpth *dpth)
{
	const char *path;

	if(blk->got!=BLK_INCOMING) return 0;
	blk->got=BLK_NOT_GOT;

	// Need to get the data for this blk from the client.
	// Set up the details of where it will be saved.
	if(!(path=dpth_protocol2_mk(dpth))) return -1;

	// FIX THIS: make dpth give us the path in a uint8 array.
	savepathstr_to_bytes(path, blk->savepath);
	blk->got_save_path=1;
	if(dpth_protocol2_incr_sig(dpth)) return -1;
	return 0;
}

static int mark_up_to_index(struct blist *blist,
	uint64_t index, struct dpth *dpth)
{
	struct blk *blk;

	// Mark everything that was not got, up to the given index.
	for(blk=blist->blk_from_champ_chooser;
	  blk && blk->index!=index; blk=blk->next)
		if(mark_not_got(blk, dpth))
			return -1;
	if(!blk)
	{
		logp("Could not find index from champ chooser: %lu\n", index);
		return -1;
	}
//logp("Found index from champ chooser: %lu\n", index);
//printf("index from cc: %d\n", index);
	blist->blk_from_champ_chooser=blk;
	return 0;
}

static int deal_with_sig_from_chfd(struct iobuf *rbuf, struct blist *blist,
	struct dpth *dpth)
{
	uint64_t fileno;
	// FIX THIS: Consider endian-ness.
	if(rbuf->len!=FILENO_LEN+SAVE_PATH_LEN)
	{
		logp("Tried to extract file number from buffer with wrong length: %u!=%u in %s\n", rbuf->len, FILENO_LEN+SAVE_PATH_LEN, __func__);
		return -1;
	}
	memcpy(&fileno, rbuf->buf, FILENO_LEN);
	if(mark_up_to_index(blist, fileno, dpth)) return -1;
	memcpy(blist->blk_from_champ_chooser->savepath,
		rbuf->buf+FILENO_LEN, SAVE_PATH_LEN);
	blist->blk_from_champ_chooser->got=BLK_GOT;
	blist->blk_from_champ_chooser->got_save_path=1;
	return 0;
}

static int deal_with_wrap_up_from_chfd(struct iobuf *rbuf, struct blist *blist,
	struct dpth *dpth)
{
	uint64_t fileno;
	// FIX THIS: Consider endian-ness.
	if(rbuf->len!=FILENO_LEN)
	{
		logp("Tried to extract file number from buffer with wrong length: %u!=%u in %s\n", rbuf->len, FILENO_LEN, __func__);
		return -1;
	}
	memcpy(&fileno, rbuf->buf, FILENO_LEN);
	if(mark_up_to_index(blist, fileno, dpth)) return -1;
	if(mark_not_got(blist->blk_from_champ_chooser, dpth)) return -1;

	return 0;
}

static int deal_with_read_from_chfd(struct asfd *chfd,
	struct blist *blist, uint64_t *wrap_up, struct dpth *dpth,
	struct conf **confs)
{
	int ret=-1;

	// Deal with champ chooser read here.
	//printf("read from cc: %s\n", chfd->rbuf->buf);
	switch(chfd->rbuf->cmd)
	{
		case CMD_SIG:
			// Get these for blks that the champ chooser has found.
			if(deal_with_sig_from_chfd(chfd->rbuf, blist, dpth))
				goto end;
			cntr_add_same(get_cntr(confs), CMD_DATA);
			break;
		case CMD_WRAP_UP:
			if(deal_with_wrap_up_from_chfd(chfd->rbuf, blist, dpth))
				goto end;
			break;
		default:
			iobuf_log_unexpected(chfd->rbuf, __func__);
			goto end;
	}
	ret=0;
end:
	iobuf_free_content(chfd->rbuf);
	return ret;
}

static struct asfd *get_asfd_from_list_by_fdtype(struct async *as,
	enum asfd_fdtype fdtype)
{
	struct asfd *a;
	for(a=as->asfd; a; a=a->next)
		if(a->fdtype==fdtype) return a;
	return NULL;
}

int backup_phase2_server_protocol2(struct async *as, struct sdirs *sdirs,
	int resume, struct conf **confs)
{
	int ret=-1;
	uint8_t end_flags=0;
	struct slist *slist=NULL;
	struct iobuf *wbuf=NULL;
	struct dpth *dpth=NULL;
	struct manios *manios=NULL;
	// This is used to tell the client that a number of consecutive blocks
	// have been found and can be freed.
	uint64_t wrap_up=0;
	struct asfd *asfd=as->asfd;
	struct asfd *chfd;
	enum protocol protocol=get_protocol(confs);
	chfd=get_asfd_from_list_by_fdtype(as, ASFD_FD_SERVER_TO_CHAMP_CHOOSER);

	logp("Phase 2 begin (recv backup data)\n");

	if(!(manios=manios_open_phase2(sdirs, protocol))
	  || !(slist=slist_alloc())
	  || !(wbuf=iobuf_alloc())
	  || !(dpth=dpth_alloc())
	  || dpth_protocol2_init(dpth,
		sdirs->data, get_int(confs[OPT_MAX_STORAGE_SUBDIRS])))
			goto end;

	if(resume && do_resume(manios->phase1, sdirs, dpth, confs))
                goto end;

	if(!manios->phase1
	  && !(manios->phase1=manio_open_phase1(sdirs->phase1data,
		"rb", protocol)))
			goto end;

	while(!(end_flags&END_BACKUP))
	{
		if(maybe_add_from_scan(manios, slist, confs))
				goto end;

		if(!wbuf->len)
		{
			if(get_wbuf_from_sigs(wbuf, slist, &end_flags, confs))
				goto end;
			if(!wbuf->len)
			{
				get_wbuf_from_files(wbuf, slist,
					manios, &end_flags);
			}
		}

		if(wbuf->len
		  && asfd->append_all_to_write_buffer(asfd, wbuf)==APPEND_ERROR)
			goto end;

		if(append_for_champ_chooser(chfd, slist->blist, end_flags))
			goto end;

		if(as->read_write(as))
		{
			logp("error in %s\n", __func__);
			goto end;
		}

		while(asfd->rbuf->buf)
		{
			if(deal_with_read(asfd->rbuf, slist, confs,
				&end_flags, dpth)) goto end;
			// Get as much out of the readbuf as possible.
			if(asfd->parse_readbuf(asfd)) goto end;
		}
		while(chfd->rbuf->buf)
		{
			if(deal_with_read_from_chfd(chfd,
				slist->blist, &wrap_up, dpth, confs)) goto end;
			// Get as much out of the readbuf as possible.
			if(chfd->parse_readbuf(chfd)) goto end;
		}

		if(write_to_changed_file(asfd, chfd, manios,
			slist, end_flags, confs))
				goto end;
	}

	// Hack: If there are some entries left after the last entry that
	// contains block data, it will not be written to the changed file
	// yet because the last entry of block data has not had
	// sb->protocol2->bend set.
	if(slist->head && slist->head->next)
	{
		slist->head=slist->head->next;
		if(write_to_changed_file(asfd, chfd, manios,
			slist, end_flags, confs))
				goto end;
	}

	if(manios_close(&manios))
		goto end;

	if(slist->blist->head)
	{
		logp("ERROR: finishing but still want block: %lu\n",
			slist->blist->head->index);
		goto end;
	}

	// Need to release the last left. There should be one at most.
	if(dpth->head && dpth->head->next)
	{
		logp("ERROR: More data locks remaining after: %s\n",
			dpth->head->save_path);
		goto end;
	}
	if(dpth_release_all(dpth)) goto end;

	ret=0;
end:
	logp("End backup\n");
	slist_free(&slist);
	iobuf_free_content(asfd->rbuf);
	iobuf_free_content(chfd->rbuf);
	// Write buffer did not allocate 'buf'. 
	if(wbuf) wbuf->buf=NULL;
	iobuf_free(&wbuf);
	dpth_free(&dpth);
	manios_close(&manios);
	return ret;
}
