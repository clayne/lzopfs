#include "lzopfs.h"

#include "BlockCache.h"
#include "FileList.h"
#include "CompressedFile.h"
#include "OpenCompressedFile.h"
#include "ThreadPool.h"
#include "PathUtils.h"
#include "GzipFile.h"

#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define FUSE_USE_VERSION 30
#include <fuse.h>

namespace {

typedef uint64_t FuseFH;

const size_t CacheSize = 1024 * 1024 * 32;
const size_t DefaultBlockFactor = 32;

struct FSData {
	FileList *files;
	ThreadPool pool;
	BlockCache cache;
	
	FSData(FileList* f) : files(f), pool(), cache(pool) {
		cache.maxSize(CacheSize);
	}
	~FSData() { delete files; }
};
FSData *fsdata() {
	return reinterpret_cast<FSData*>(fuse_get_context()->private_data);
}


void except(std::runtime_error& e) {
	fprintf(stderr, "%s: %s\n", typeid(e).name(), e.what());
	exit(1);
}

extern "C" void *lf_init(struct fuse_conn_info *conn
#if FUSE_MAJOR_VERSION >= 3
	, struct fuse_config *cfg
#endif
) {
	void *priv = fuse_get_context()->private_data;
	return new FSData(reinterpret_cast<FileList*>(priv));
}

extern "C" int lf_getattr(const char *path, struct stat *stbuf
#if FUSE_MAJOR_VERSION >= 3
	, struct fuse_file_info *fi
#endif
) {
	memset(stbuf, 0, sizeof(*stbuf));
	
	CompressedFile *file;
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 3;
	} else if ((file = fsdata()->files->find(path))) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = file->uncompressedSize();
	} else {
		return -ENOENT;
	}
	return 0;
}

struct DirFiller {
	void *buf;
	fuse_fill_dir_t filler;
	DirFiller(void *b, fuse_fill_dir_t f) : buf(b), filler(f) { }
	void operator()(const std::string& path) {
		filler(buf, path.c_str() + 1, NULL, 0
#if FUSE_MAJOR_VERSION >= 3
			, (fuse_fill_dir_flags)0
#endif
		);
	}
};

extern "C" int lf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi
#if FUSE_MAJOR_VERSION >= 3
		, fuse_readdir_flags flags
#endif
) {
	if (strcmp(path, "/") != 0)
		return -ENOENT;
	
	DirFiller dirFiller(buf, filler);
	dirFiller("/.");
	dirFiller("/..");
	fsdata()->files->forNames(dirFiller);
	return 0;
}

extern "C" int lf_open(const char *path, struct fuse_file_info *fi) {
	CompressedFile *file;
	if (!(file = fsdata()->files->find(path)))
		return -ENOENT;
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;
	
	try {
		fi->fh = FuseFH(new OpenCompressedFile(file, O_RDONLY));
		return 0;
	} catch (FileHandle::Exception& e) {
		return e.error_code;
	}
}

extern "C" int lf_release(const char *path, struct fuse_file_info *fi) {
	delete reinterpret_cast<OpenCompressedFile*>(fi->fh);
	fi->fh = 0;
	return 0;
}

extern "C" int lf_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	int ret = -1;
	try {
		ret = reinterpret_cast<OpenCompressedFile*>(fi->fh)->read(
			fsdata()->cache, buf, size, offset);
	} catch (std::runtime_error& e) {
		except(e);
	}
	return ret;
}


typedef std::vector<std::string> paths_t;
struct OptData {
	const char *nextSource;
	paths_t* files;
	
	unsigned blockFactor;
	const char *indexRoot;
};

static struct fuse_opt lf_opts[] = {
	{ "--block-factor=%lu", offsetof(OptData, blockFactor), 0 },
	{ "--index-root=%s", offsetof(OptData, indexRoot), 0 },
	{NULL, -1U, 0},
};

extern "C" int lf_opt_proc(void *data, const char *arg, int key,
		struct fuse_args *outargs) {
	OptData *optd = reinterpret_cast<OptData*>(data);
	if (key == FUSE_OPT_KEY_NONOPT) {
		if (optd->nextSource) {
			try {
				fprintf(stderr, "%s\n", optd->nextSource);
				optd->files->push_back(PathUtils::realpath(optd->nextSource));
			} catch (std::runtime_error& e) {
				except(e);
			}
		}
		optd->nextSource = arg;
		return 0;
	}
	return 1;
}

} // anon namespace

int main(int argc, char *argv[]) {
	try {
		umask(0);
		
		struct fuse_operations ops;
		memset(&ops, 0, sizeof(ops));
		ops.getattr = lf_getattr;
		ops.readdir = lf_readdir;
		ops.open = lf_open;
		ops.release = lf_release;
		ops.read = lf_read;
		ops.init = lf_init;
		
		// FIXME: help with options?
		paths_t files;
		OptData optd = { 0, &files, DefaultBlockFactor, "" };
		struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
		fuse_opt_parse(&args, &optd, lf_opts, lf_opt_proc);
		if (optd.nextSource)
			fuse_opt_add_arg(&args, optd.nextSource);
		
		OpenParams params(CacheSize, optd.indexRoot, optd.blockFactor);
		
		FileList *flist = new FileList(params);
		for (paths_t::const_iterator iter = files.begin(); iter != files.end();
				++iter) {
			flist->add(*iter);
		}
		
		fprintf(stderr, "Ready\n");
		return fuse_main(args.argc, args.argv, &ops, flist);
	} catch (std::runtime_error& e) {
		except(e);
	}
}
