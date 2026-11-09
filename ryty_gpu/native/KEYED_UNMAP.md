Native unmap uses aligned-key lookups for sparse removals in its page and fault tracking
containers when fewer than one quarter as many lookups as entries are needed. Larger or
invalid ranges use the original scan. Set `RYGT_NATIVE_KEYED_UNMAP=0` before process start
to use the original scans for an A/B comparison. The choice is sampled on first use; restart
the process after changing it. This affects bookkeeping only, not mapping or synchronization.
