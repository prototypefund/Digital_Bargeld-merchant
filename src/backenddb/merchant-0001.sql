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

-- Check patch versioning is in place.
SELECT _v.register_patch('merchant-0001', NULL, NULL);


CREATE TABLE IF NOT EXISTS merchant_orders
  (order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,contract_terms BYTEA NOT NULL
  ,timestamp INT8 NOT NULL
  ,PRIMARY KEY (order_id, merchant_pub)
  );

-- Offers we made to customers
CREATE TABLE IF NOT EXISTS merchant_contract_terms
   (order_id VARCHAR NOT NULL
   ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
   ,contract_terms BYTEA NOT NULL
   ,h_contract_terms BYTEA NOT NULL CHECK (LENGTH(h_contract_terms)=64)
   ,timestamp INT8 NOT NULL
   ,row_id BIGSERIAL UNIQUE
   ,paid boolean DEFAULT FALSE NOT NULL
   ,PRIMARY KEY (order_id, merchant_pub)
   ,UNIQUE (h_contract_terms, merchant_pub)
   );

-- Table with the proofs for each coin we deposited at the exchange
CREATE TABLE IF NOT EXISTS merchant_deposits
  (h_contract_terms BYTEA NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,exchange_url VARCHAR NOT NULL
  ,amount_with_fee_val INT8 NOT NULL
  ,amount_with_fee_frac INT4 NOT NULL
  ,deposit_fee_val INT8 NOT NULL
  ,deposit_fee_frac INT4 NOT NULL
  ,refund_fee_val INT8 NOT NULL
  ,refund_fee_frac INT4 NOT NULL
  ,wire_fee_val INT8 NOT NULL
  ,wire_fee_frac INT4 NOT NULL
  ,signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)
  ,exchange_proof BYTEA NOT NULL
  ,PRIMARY KEY (h_contract_terms, coin_pub)
  ,FOREIGN KEY (h_contract_terms, merchant_pub) REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub)
  );

CREATE TABLE IF NOT EXISTS merchant_proofs
  (exchange_url VARCHAR NOT NULL
  ,wtid BYTEA CHECK (LENGTH(wtid)=32)
  ,execution_time INT8 NOT NULL
  ,signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)
  ,proof BYTEA NOT NULL
  ,PRIMARY KEY (wtid, exchange_url)
  );

-- Note that h_contract_terms + coin_pub may actually be unknown to
-- us, e.g. someone else deposits something for us at the exchange.
-- Hence those cannot be foreign keys into deposits/transactions!
CREATE TABLE IF NOT EXISTS merchant_transfers
  (h_contract_terms BYTEA NOT NULL
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,wtid BYTEA NOT NULL CHECK (LENGTH(wtid)=32)
  ,PRIMARY KEY (h_contract_terms, coin_pub)
  );
CREATE INDEX IF NOT EXISTS merchant_transfers_by_coin
  ON merchant_transfers
  (h_contract_terms
  ,coin_pub);
CREATE INDEX IF NOT EXISTS merchant_transfers_by_wtid
  ON merchant_transfers
  (wtid);

CREATE TABLE IF NOT EXISTS exchange_wire_fees
  (exchange_pub BYTEA NOT NULL CHECK (LENGTH(exchange_pub)=32)
  ,h_wire_method BYTEA NOT NULL CHECK (LENGTH(h_wire_method)=64)
  ,wire_fee_val INT8 NOT NULL
  ,wire_fee_frac INT4 NOT NULL
  ,closing_fee_val INT8 NOT NULL
  ,closing_fee_frac INT4 NOT NULL
  ,start_date INT8 NOT NULL
  ,end_date INT8 NOT NULL
  ,exchange_sig BYTEA NOT NULL CHECK (LENGTH(exchange_sig)=64)
  ,PRIMARY KEY (exchange_pub,h_wire_method,start_date,end_date)
  );

CREATE TABLE IF NOT EXISTS merchant_refunds
  (rtransaction_id BIGSERIAL UNIQUE
  ,merchant_pub BYTEA NOT NULL
  ,h_contract_terms BYTEA NOT NULL
  ,coin_pub BYTEA NOT NULL
  ,reason VARCHAR NOT NULL
  ,refund_amount_val INT8 NOT NULL
  ,refund_amount_frac INT4 NOT NULL
  ,FOREIGN KEY (h_contract_terms, coin_pub) REFERENCES merchant_deposits (h_contract_terms, coin_pub)
  ,FOREIGN KEY (h_contract_terms, merchant_pub) REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub)
  );

-- balances of the reserves available for tips
CREATE TABLE IF NOT EXISTS merchant_tip_reserves
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,expiration INT8 NOT NULL
  ,balance_val INT8 NOT NULL
  ,balance_frac INT4 NOT NULL
  ,PRIMARY KEY (reserve_priv)
  );

-- table where we remember when tipping reserves where established / enabled
CREATE TABLE IF NOT EXISTS merchant_tip_reserve_credits
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,credit_uuid BYTEA UNIQUE NOT NULL CHECK (LENGTH(credit_uuid)=64)
  ,timestamp INT8 NOT NULL
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,PRIMARY KEY (credit_uuid)
  );

-- tips that have been authorized
CREATE TABLE IF NOT EXISTS merchant_tips
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,tip_id BYTEA NOT NULL CHECK (LENGTH(tip_id)=64)
  ,exchange_url VARCHAR NOT NULL
  ,justification VARCHAR NOT NULL
  ,extra BYTEA NOT NULL
  ,timestamp INT8 NOT NULL
  ,amount_val INT8 NOT NULL /* overall tip amount */
  ,amount_frac INT4 NOT NULL
  ,left_val INT8 NOT NULL /* tip amount not yet picked up */
  ,left_frac INT4 NOT NULL
  ,PRIMARY KEY (tip_id)
  );

-- tips that have been picked up
CREATE TABLE IF NOT EXISTS merchant_tip_pickups
  (tip_id BYTEA NOT NULL REFERENCES merchant_tips (tip_id) ON DELETE CASCADE
  ,pickup_id BYTEA NOT NULL CHECK (LENGTH(pickup_id)=64)
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,PRIMARY KEY (pickup_id)
  );

-- sessions and their order_id/fulfillment_url mapping
CREATE TABLE IF NOT EXISTS merchant_session_info
  (session_id VARCHAR NOT NULL
  ,fulfillment_url VARCHAR NOT NULL
  ,order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,timestamp INT8 NOT NULL
  ,PRIMARY KEY (session_id, fulfillment_url, merchant_pub)
  ,UNIQUE (session_id, fulfillment_url, order_id, merchant_pub)
  );

-- Complete transaction
COMMIT;
