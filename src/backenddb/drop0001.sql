--
-- This file is part of TALER
-- Copyright (C) 2014--2020 Taler Systems SA
--
-- TALER is free software; you can redistribute it and/or modify it under the
-- terms of the GNU General Public License as published by the Free Software
-- Foundation; either version 3, or (at your option) any later version.
--
-- TALER is distributed in the hope that it will be useful, but WITHOUT ANY
-- WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
-- A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License along with
-- TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
--

-- Everything in one big transaction
BEGIN;

-- This script DROPs all of the tables we create, including the
-- versioning schema!
--
-- Unlike the other SQL files, it SHOULD be updated to reflect the
-- latest requirements for dropping tables.

-- Drops for 0001.sql

DROP TABLE IF EXISTS merchant_exchange_wire_fees CASCADE;
DROP TABLE IF EXISTS merchant_exchange_signing_keys CASCADE;
DROP TABLE IF EXISTS merchant_instances CASCADE;
DROP TABLE IF EXISTS merchant_keys CASCADE;
DROP TABLE IF EXISTS merchant_accounts CASCADE;
DROP TABLE IF EXISTS merchant_inventory CASCADE;
DROP TABLE IF EXISTS merchant_inventory_locks CASCADE;
DROP TABLE IF EXISTS merchant_accounts CASCADE;
DROP TABLE IF EXISTS merchant_orders CASCADE;
DROP TABLE IF EXISTS merchant_order_locks CASCADE;
DROP TABLE IF EXISTS merchant_contract_terms CASCADE;
DROP TABLE IF EXISTS merchant_deposits CASCADE;
DROP TABLE IF EXISTS merchant_refunds CASCADE;
DROP TABLE IF EXISTS merchant_credits CASCADE;
DROP TABLE IF EXISTS merchant_transfer_signatures CASCADE;
DROP TABLE IF EXISTS merchant_transfer_by_coin CASCADE;
DROP TABLE IF EXISTS merchant_tip_reserves CASCADE;
DROP TABLE IF EXISTS merchant_tip_reserve_keys CASCADE;
DROP TABLE IF EXISTS merchant_tips CASCADE;
DROP TABLE IF EXISTS merchant_tip_pickups CASCADE;
DROP TABLE IF EXISTS merchant_tip_pickup_signatures CASCADE;

-- Drop versioning (0000.sql)
DROP SCHEMA IF EXISTS _v CASCADE;

-- And we're out of here...
COMMIT;
