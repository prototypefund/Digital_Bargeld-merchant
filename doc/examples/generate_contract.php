function make_contract($transaction_id, $now) @{
 $contract =
  array (
   'amount' => array (
    'value' => 1,
    'fraction' => 0,
    'currency' => "KUDOS"),
   'max_fee' => array (
    'value' => 0,
    'fraction' => 50000,
    'currency' => "KUDOS"),
   'transaction_id' => $transaction_id,
   'products' => array (
     array (
      'description' => "Donation to charity program",
      'quantity' => 1,
      'price' => array (
       'value' => 1,
       'fraction' => 0,
       'currency' => "KUDOS"),
      'product_id' => 0,
      'taxes' => array(), // No taxes for donations
      'delivery_date' => "/Date(" . $now->getTimestamp() . ")/",
      'delivery_location' => 'LNAME1')),
      'timestamp' => "/Date(" . $now->getTimestamp() . ")/",
      'expiry' =>
       "/Date(" . $now->add(new DateInterval('P2W'))->getTimestamp() . ")/",
      'refund_deadline' =>
       "/Date(" . $now->add(new DateInterval('P3M'))->getTimestamp() . ")/",
      'repurchase_correlation_id' => '',
      'fulfillment_url' =>
       "https://shop.com/fulfillment?"
       . "transaction_id=$transaction_id&timestamp=$now",
      'merchant' => array (
       'address' => 'LNAME1',
       'name' => "Charity donations shop",
       'jurisdiction' => 'LNAME2'),
      'locations' => array (
       'LNAME1' =>
        array (
         'country' => 'Shop Country',
         'city' => 'Shop City',
         'state' => 'Shop State',
       	 'region' => 'Shop Region',
       	 'province' => 'Shop Province',
       	 'ZIP code' => 4908,
         'street' => 'Shop street',
       	 'street number' => 20),
       'LNAME2' => array (
         'country' => 'Legal Country',
         'city' => Legal City',
         'state' => 'Legal State',
         'region' => 'Legal Region',
         'province' => 'Legal Province',
         'ZIP code' => 4908)));
@}


/* this variable is the JSON of a contract proposal,
 see https://api.taler.net/api-merchant.html#post--contract
 the second parameter is the transaction id */
$transaction_id = rand(1,90000); // simplified, do not do this!
$proposal = make_contract($transaction_id, new DateTime('now'));

# Here the frontend POSTs the proposal to the backend
$response = post_to_backend("/contract", $proposal);

if (200 != $response->getResponseCode()) @{
  echo json_encode(array(
    'error' => "internal error",
    'hint' => "failed to generate contract",
    'detail' => $resp->body->toString()
    ), JSON_PRETTY_PRINT);
  return;
@}
echo $response->body;
