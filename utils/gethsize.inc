static int test_access(int h, __u64 sec)
{
	char c;
	__u64 r;
#ifdef use_llseek
	loff_t l;
#endif
#ifdef use_llseek
	r = _llseek(h, sec >> 32, sec, &l, SEEK_SET);
#else
	r = lseek(h, sec, SEEK_SET);
#endif
	if (r == -1) return 0;
	r = read(h, &c, 1);
	return r == 1;
}

static __u64 gethsize(int h)
{
	__u64 b;
	__u64 size = 1;
	struct stat st;
	test_access(h, 0);	/* this may do media probing */
	if (__likely(!fstat(h, &st))) {
		if (__likely(st.st_size != 0)) return st.st_size;
	}
	while (size && test_access(h, size)) size <<= 1;
	if (size) size >>= 1;
	else size = (__u64)1 << 63;
	for (b = size >> 1; b; b >>= 1) if (test_access(h, size | b)) size |= b;
	size++;
	return size;
}

