      "DROP TABLE IF EXISTS merchant_transfers CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_deposits CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_transactions CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_proofs CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_contract_terms CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_refunds CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS exchange_wire_fees CASCADE;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE IF EXISTS merchant_tips CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_tip_pickups CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_tip_reserve_credits CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_tip_reserves CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_orders CASCADE;"),
    GNUNET_PQ_make_try_execute (
      "DROP TABLE IF EXISTS merchant_session_info CASCADE;"),
