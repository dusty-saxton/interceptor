// This firmware never uses dynamic memory allocation (malloc/free) - this
// stub exists only to satisfy the linker's reference to _sbrk pulled in by
// newlib's reentrant heap support. If it's ever actually called at runtime
// (it shouldn't be), it reports "out of memory" rather than doing anything
// unsafe.
void *_sbrk(int incr)
{
    (void)incr;
    return (void *)-1;
}
