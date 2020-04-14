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


---------------- Exchange information ---------------------------

CREATE TABLE IF NOT EXISTS merchant_exchange_wire_fees
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
COMMENT ON TABLE merchant_exchange_wire_fees
 IS 'Here we store proofs of the wire fee structure of the various exchanges';
COMMENT ON COLUMN merchant_exchange_wire_fees.exchange_pub
 IS 'FIXME: Is this a MASTER public key or an online signing key? Clarify!';


-------------------------- Instances  ---------------------------

CREATE TABLE IF NOT EXISTS merchant_instances
  (merchant_priv BYTEA CHECK (LENGTH(merchant_priv)=32),
   merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32),
  ,merchant_name VARCHAR NOT NULL
  ,location BYTEA NOT NULL
  ,jurisdiction BYTEA NOT NULL
  ,PRIMARY KEY (reserve_pub)
  );
COMMENT ON TABLE merchant_instances
  IS 'all the instances supported by this backend';
COMMENT ON COLUMN merchant_instances.merchant_priv
  IS 'note that the NOT NULL constraint is omitted here (instance delete vs. purge). However, because libgnunetpq does not support NULL columns well, we MAY want to move the private keys into another table instead and simply there delete the entire row.';
COMMENT ON COLUMN merchant_instances.merchant_name
  IS 'legal name of the merchant as a simple string (required)';
COMMENT ON COLUMN merchant_instances.location
  IS 'physical location of the merchant as a Location in JSON format (required)';
COMMENT ON COLUMN merchant_instances.jurisdiction
  IS 'jurisdiction of the merchant as a Location in JSON format (required)';


CREATE TABLE IF NOT EXISTS merchant_instance_accounts
  (merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32),
  ,h_wire BYTEA NOT NULL CHECK (LENGTH(h_wire)=64), -- or did we use a shorter hash here?
  ,salt BYTEA NOT NULL CHECK (LENGTH(salt)=64), -- or did we use a shorter salt here?
  ,active boolean NOT NULL
  ,payto_uri VARCHAR NOT NULL CHECK,
  ,PRIMARY KEY (merchant_pub,h_wire)
  ,FOREIGN KEY (merchant_pub)
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_accounts
  IS 'bank accounts of the instances';
COMMENT ON COLUMN merchant_accounts.h_wire
  IS 'salted hash of payto_uri';
COMMENT ON COLUMN merchant_accounts.salt
  IS 'salt used when hashing payto_uri into h_wire';
COMMENT ON COLUMN merchant_accounts.payto_uri
  IS 'payto URI of a merchant bank account';
COMMENT ON COLUMN merchant_instances.active
  IS 'true if we actively use this bank account, false if it is just kept around for older contracts to refer to';


-------------------------- Inventory  ---------------------------


CREATE TABLE IF NOT EXISTS merchant_inventory
  (product_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,description VARCHAR NOT NULL
  ,description_i18n BYTEA NOT NULL
  ,unit VARCHAR NOT NULL
  ,image BYTEA NOT NULL
  ,taxes BYTEA NOT NULL
  ,price_val INT8 NOT NULL
  ,price_frac INT4 NOT NULL
  ,total_stock BIGINT NOT NULL
  ,total_sold BIGINT NOT NULL
  ,total_lost BIGINT NOT NULL
  ,total_locked BIGINT NOT NULL
  ,location BYTEA NOT NULL
  ,next_restock INT8 NOT NULL
  ,PRIMARY KEY (product_id, merchant_pub)
  ,FOREIGN KEY (merchant_pub)
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_inventory
  IS 'products offered by the merchant (may be incomplete, frontend can override)';
COMMENT ON COLUMN merchant_inventory.description
  IS 'Human-readable product description';
COMMENT ON COLUMN merchant_inventory.description_i18n
  IS 'JSON map from IETF BCP 47 language tags to localized descriptions';
COMMENT ON COLUMN merchant_inventory.unit
  IS 'Unit of sale for the product (liters, kilograms, packages)';
COMMENT ON COLUMN merchant_inventory.image
  IS 'NOT NULL, but can be 0 bytes; must contain an ImageDataUrl';
COMMENT ON COLUMN merchant_inventory.taxes
  IS 'JSON array containing taxes the merchant pays, must be JSON, but can be just "[]"';
COMMENT ON COLUMN merchant_inventory.price_val
  IS 'Current price of one unit of the product';
COMMENT ON COLUMN merchant_inventory.total_stock
  IS 'A value of -1 is used for unlimited (eletronic good), may never be lowered';
COMMENT ON COLUMN merchant_inventory.total_sold
  IS 'Number of products sold, must be below total_stock, non-negative, may never be lowered';
COMMENT ON COLUMN merchant_inventory.total_lost
  IS 'Number of products that used to be in stock but were lost (spoiled, damaged), may never be lowered';
COMMENT ON COLUMN merchant_inventory.total_locked
  IS 'Number of this product that is currently locked by shopping carts awaiting sale';
COMMENT ON COLUMN merchant_inventory.location
  IS 'JSON formatted Location of where the product is stocked';
COMMENT ON COLUMN merchant_inventory.next_restock
  IS 'GNUnet absolute time indicating when the next restock is expected. 0 for unknown.';


CREATE TABLE IF NOT EXISTS merchant_inventory_locks
  (product_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,lock_uuid BYTEA NOT NULL -- FIXME: length constraint?
  ,total_locked BIGINT NOT NULL
  ,expiration TIMESTAMP NOT NULL,
  ,FOREIGN KEY (product_id, merchant_pub)
     REFERENCES merchant_inventory (product_id, merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_inventory_locks
  IS 'locks on inventory helt by shopping carts';
COMMENT ON TABLE merchant_inventory_locks.total_locked
  IS 'how many units of the product does this lock reserve';
COMMENT ON TABLE merchant_inventory_locks.expiration
  IS 'when does this lock automatically expire (if no order is created)';


---------------- Orders and contracts ---------------------------

CREATE TABLE IF NOT EXISTS merchant_orders
  (order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,contract_terms BYTEA NOT NULL
  ,timestamp INT8 NOT NULL
  ,PRIMARY KEY (order_id, merchant_pub)
  ,FOREIGN KEY (merchant_pub)
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_orders
  IS 'Orders we offered to a customer, but that have not yet been claimed';
COMMENT ON COLUMN merchnat_orders.contract_terms
  IS 'Claiming changes the contract_terms, hence we have no hash of the terms in this table';
COMMENT ON COLUMN merchant_orders.merchant_pub
  IS 'Identifies the instance offering the contract';
COMMENT ON COLUMN merchnat_orders.timestamp
  IS 'Timestamp from the order. FIXME: Likely a bad choice, should be replaced with its expiration';
-- FIXME: timestamp seems useless, expiration would be good for garbage collection!
-- CREATE INDEX IF NOT EXISTS merchant_orders_by_expiration
--   ON merchant_orders
--   (expiration); // once timestamp => expiration!

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
  ,FOREIGN KEY (merchant_pub)
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_contract_terms
  IS 'Contracts are orders that have been claimed by a wallet';
COMMENT ON COLUMN merchant_contract_terms.order_id
  IS 'Not a foreign key into merchant_orders because paid contracts persist after expiration';
COMMENT ON COLUMN merchant_contract_terms.merchant_pub
  IS 'Identifies the instance offering the contract';
COMMENT ON COLUMN merchant_contract_terms.contract_terms
  IS 'These contract terms include the wallet nonce';
COMMENT ON COLUMN merchant_contract_terms.h_contract_terms
  IS 'Hash over contract_terms';
COMMENT ON COLUMN merchant_contract_terms.paid
  IS 'true implies the customer paid for this contract';
COMMENT ON COLUMN merchant_contract_terms.timestamp
  IS 'Timestamp from the order. FIXME: Likely a bad choice, should be replaced with its expiration'; -- == pay_deadline
-- FIXME: consider also explicitly exposing the 'refund_deadline'
-- FIXME: consider adding a BIGSERIAL contract_id to use instead of
--        merchant_pub+h_contract_terms
--        when referencing this contract in other tables (session_info, deposits, transfers)


-- CREATE INDEX IF NOT EXISTS merchant_contract_terms_by_expiration
--   ON merchant_contract_terms
--   (expiration); // once timestamp => expiration!




---------------- Payment and refunds ---------------------------

CREATE TABLE IF NOT EXISTS merchant_session_info
  (session_id VARCHAR NOT NULL
  ,fulfillment_url VARCHAR NOT NULL
  ,order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,timestamp INT8 NOT NULL
  ,PRIMARY KEY (session_id, fulfillment_url, merchant_pub)
-- FIXME: I am confused why this even *IS* a primary key.
  ,FOREIGN KEY (order_id, merchant_pub)
     REFERENCES merchant_orders (order_id, merchant_pub)
-- FIXME: if this is for session-bound payments,
--        we need to reference merchant_contracts as
--        the *order* may be GCed after payment but the
--        session-bound payment mechanism should continue to work!
  ,UNIQUE (session_id, fulfillment_url, order_id, merchant_pub)
-- FIXME: isn't this redundant with the (confusing) PRIMARY KEY?
  );
-- FIXME: Support for #5853 (limit session in number and duration)
--        should be supported 'somewhere' here.
COMMENT ON TABLE merchant_session_info
  IS 'sessions and their order_id/fulfillment_url mapping';
COMMENT ON COLUMN merchant_session_info.fulfillment_url
  IS 'FIXME: Unclear why the fulfillment URL is in this table';
COMMENT ON COLUMN merchant_session_info.order_id
  IS 'FIXME: Why use merchant_pub+order_id here, instead of a say a contract_id?';

CREATE TABLE IF NOT EXISTS merchant_deposits
  (h_contract_terms BYTEA NOT NULL
  ,merchant_pub BYTEA NOT NULL
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
  ,exchange_proof BYTEA NOT NULL -- FIXME: rename to deposit_confirmation? Why full JSON? (seems we only need the exchange_sig/exchange_pub and the exchange's timestamp and our h_wire, the rest we have!) [Except maybe: also keep master_sig?]
  ,PRIMARY KEY (h_contract_terms, coin_pub)
  ,FOREIGN KEY (h_contract_terms, merchant_pub)
     REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_deposits
  IS 'Table with the deposit confirmations for each coin we deposited at the exchange';
COMMENT ON COLUMN merchant_deposits.signkey_pub
  IS 'FIXME: Is this the exchange_pub? If so, consider renaming! Also, should it not be with exchange_url a FOREIGN KEY into merchant_proofs?';
COMMENT ON COLUMN merchant_deposits.wire_fee_val
  IS 'FIXME: Can we not rather get this via merchant_exchange_wire_fees somehow? (may be too complicated)';
-- FIXME: maybe want a column to indicate that we received payment for this deposit
-- from the exchange (for easy/fast filtering for deposits that lack wire transfers)


CREATE TABLE IF NOT EXISTS merchant_refunds
  (rtransaction_id BIGSERIAL UNIQUE
  ,merchant_pub BYTEA NOT NULL
  ,h_contract_terms BYTEA NOT NULL
  ,coin_pub BYTEA NOT NULL
  ,reason VARCHAR NOT NULL
  ,refund_amount_val INT8 NOT NULL
  ,refund_amount_frac INT4 NOT NULL
  ,FOREIGN KEY (h_contract_terms, coin_pub)
     REFERENCES merchant_deposits (h_contract_terms, coin_pub) ON DELETE CASCADE
  ,FOREIGN KEY (h_contract_terms, merchant_pub)
     REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub) ON DELETE CASCADE
  ,PRIMARY KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
  );
COMMENT ON TABLE merchant_deposits
  IS 'Refunds approved by the merchant (backoffice) logic';
COMMENT ON COLUMN merchant_refunds.rtransaction_id
  IS 'Needed for uniqueness in case a refund is increased for the same order';
-- FIXME: do we really want rtransaction_id as BIGSERIAL UNIQUE?
--        this exposes # of refunds granted to clients!!!

CREATE TABLE IF NOT EXISTS merchant_refund_proofs
  (rtransaction_id BIGSERIAL UNIQUE
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,h_contract_terms BYTEA NOT NULL CHECK (LENGTH(h_contract_terms)=64)
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,exchange_sig BYTEA NOT NULL CHECK (LENGTH(exchange_sig)=64)
  ,exchange_pub BYTEA NOT NULL CHECK (LENGTH(exchange_pub)=32)
  ,FOREIGN KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
     REFERENCES merchant_refunds (h_contract_terms, merchant_pub, coin_pub, rtransaction_id) ON DELETE CASCADE
  ,PRIMARY KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
);
COMMENT ON TABLE merchant_refund_proofs
  IS 'Refunds confirmed by the exchange (not all approved refunds are grabbed by the wallet)';
-- FIXME: rtransaction_id as BIGSERIAL UNIQUE should suffice, rest of information
--        in the FOREIGN KEY is duplicated for no good reason.


-------------------- Wire transfers ---------------------------

-- FIXME: likely need another table for when the merchant administrator
-- informs us about a wire transfer it received!

-- FIXME: terrible table name, this is about transfer tracking responses from the exchange!
CREATE TABLE IF NOT EXISTS merchant_proofs
  (exchange_url VARCHAR NOT NULL
  ,wtid BYTEA CHECK (LENGTH(wtid)=32)
  ,execution_time INT8 NOT NULL
  ,signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)
  ,proof BYTEA NOT NULL
  ,PRIMARY KEY (wtid, exchange_url)
  );

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
COMMENT ON TABLE merchant_transfers
  IS 'Mapping of deposits to wire transfers and vice versa';
COMMENT ON COLUMN merchant_transfers.coin_pub
  IS 'Note that the coin_pub/h_contract_terms can theoretically be unknown to us if someone else deposited for us at the exchange. Hence those cannot be foreign keys into the merchant_deposits table.';


-------------------------- Tipping ---------------------------


CREATE TABLE IF NOT EXISTS merchant_tip_reserves
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,expiration INT8 NOT NULL
  ,balance_val INT8 NOT NULL
  ,balance_frac INT4 NOT NULL
  ,PRIMARY KEY (reserve_priv)
--  ,FOREIGN KEY (merchant_pub)
--    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_tip_reserves
  IS 'balances of the reserves available for tips';
COMMENT ON COLUMN merchant_tip_reserves.expiration
  IS 'FIXME: EXCHANGE API needs to tell us when reserves close if we are to compute this';
-- FIXME: needs a merchant_pub column!
-- FIXME: needs an exchange_url column!
-- FIXME: needs amount columns on initial balance (according to merchant),
--                            and initial balance (according to exchange / reserve status)
-- FIXME: needs amount columns on committed and picked up amounts!
-- FIXME: needs amount columns on amount affected by closing?
-- FIXME: maybe use reserve_pub here, and have another table with reserve_priv,
--        to support delete vs. purge just like with instances?
--        (delete = only remove private key, purge = forget everything, including
--         information about all tips that were picked up)


-- table where we remember when tipping reserves where established / enabled
CREATE TABLE IF NOT EXISTS merchant_tip_reserve_credits
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,credit_uuid BYTEA UNIQUE NOT NULL CHECK (LENGTH(credit_uuid)=64)
  ,timestamp INT8 NOT NULL
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,PRIMARY KEY (credit_uuid)
  );
-- FIXME: this table can likely be folded with merchant_tip_reserves
-- FIXME: credit_uuid no longer makes sense, merchant_pub is now always used, right?
--        (was needed in legacy, because then we allowed reserve_pub to be repeated!)


CREATE TABLE IF NOT EXISTS merchant_tips
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32)
  ,tip_id BYTEA NOT NULL CHECK (LENGTH(tip_id)=64)
  ,exchange_url VARCHAR NOT NULL
  ,justification VARCHAR NOT NULL
  ,extra BYTEA NOT NULL
  ,timestamp INT8 NOT NULL
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,left_val INT8 NOT NULL
  ,left_frac INT4 NOT NULL
  ,PRIMARY KEY (tip_id)
  );
COMMENT ON TABLE merchant_tips
  IS 'tips that have been authorized';
COMMENT ON COLUMN merchant_tips.amount_val
  IS 'Overall tip amount';
COMMENT ON COLUMN merchant_tips.left_val
  IS 'Tip amount not yet picked up';
COMMENT ON COLUMN merchant_tips.reserve_priv
  IS 'Seems like a bad idea to have the reserve_priv here. Why not add a reserve_pub?';
-- NOTE: reserve_pub cannot be FOREIGN KEY, as reserve MAY be deleted (but tip continues!)
-- NOTE: also add merchant_pub to anchor tip to instance?
COMMENT ON COLUMN merchant_tips.extra
  IS 'FIXME: Used to be the response from the exchange (??), needs fix!';
COMMENT ON COLUMN merchant_tips.timestamp
  IS 'FIXME: bad practice to use SQL keyword for column name; also: use expiration for GC?';
-- FIXME: timestamp seems useless, expiration would be good for garbage collection!
-- CREATE INDEX IF NOT EXISTS merchant_tips_by_expiration
--   ON merchant_tips
--   (expiration); // once timestamp => expiration!
-- FIXME: for DELETION, might be good to have an easy way to filter
--        between tips offered that were never picked up, and
--        tips that were picked up (for which the original expiration
--        does NOT matter anymore!)


CREATE TABLE IF NOT EXISTS merchant_tip_pickups
  (tip_id BYTEA NOT NULL REFERENCES merchant_tips (tip_id) ON DELETE CASCADE
  ,pickup_id BYTEA NOT NULL CHECK (LENGTH(pickup_id)=64)
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,PRIMARY KEY (pickup_id)
  );
COMMENT ON TABLE merchant_tip_pickups
  IS 'tips that have been picked up';
-- FIXME: should store exchange response here somewhere (or in extra table)!
COMMENT ON COLUMN merchant_tips.amount_val
  IS 'total transaction cost for all coins including withdraw fees';


-- Complete transaction
COMMIT;
