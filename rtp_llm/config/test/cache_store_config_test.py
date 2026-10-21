import pickle
import unittest

from rtp_llm.ops.libth_transformer_config import CacheStoreConfig


class CacheStoreConfigTest(unittest.TestCase):
    def test_default_transport(self):
        config = CacheStoreConfig()
        self.assertEqual(config.cache_store_transport, "")

    def test_pickle_preserves_transport(self):
        config = CacheStoreConfig()
        config.cache_store_transport = "hixl"
        self.assertIn("cache_store_transport: hixl", config.to_string())
        restored = pickle.loads(pickle.dumps(config))
        self.assertEqual(restored.cache_store_transport, "hixl")

    def test_legacy_baseline_pickle_keeps_default_transport(self):
        config = CacheStoreConfig()
        state = config.__getstate__()[:20]
        restored = CacheStoreConfig.__new__(CacheStoreConfig)
        restored.__setstate__(state)
        self.assertEqual(restored.cache_store_transport, "")
        self.assertEqual(restored.thread_count, config.thread_count)
        self.assertEqual(
            restored.cache_store_tcp_anet_rpc_queue_num,
            config.cache_store_tcp_anet_rpc_queue_num,
        )

    def test_rejects_invalid_pickle_state(self):
        config = CacheStoreConfig()
        state = config.__getstate__()
        for invalid_state in ((), state[:19], state + (None,)):
            with self.subTest(length=len(invalid_state)):
                restored = CacheStoreConfig.__new__(CacheStoreConfig)
                with self.assertRaises(RuntimeError):
                    restored.__setstate__(invalid_state)


if __name__ == "__main__":
    unittest.main()
