<html>
 <head>
  <title>Donation Fulfillment</titile>
 </head>
 <body>
  <?php
   # At first, check if the user has already paid for the product.
   # If so, deliver the product.
   session_start();
   if (! isset($_SESSION['paid']))@{
    # set as pending
    $_SESSION['paid'] = false;
   @}
   else@{
    if($_SESSION['paid'])@{
     echo "<p>Thanks for your donation!</p>";
     return;
   @}
    else@{
      # Generate page to show for payments with credit cards instead.
      echo '<form action="/cc-payment">
      Name<br> <input type="text"></input><br>
      CC number<br> <input type="text"></input><br>
      Cvv2 code<br> <input type="text"></input><br>
      <input type="submit"></input>
      </form>';
      return;
   @}
  @}

  # Reconstruct the contract
  $rec_proposal = make_contract($_GET['transaction_id'], $_GET['timestamp']);
  # $response corresponds to the specification at:
  # https://api.taler.net/api-merchant.html#offer
  $response = post_to_backend("/contract", $rec_proposal);

  http_response_code (402);
# FIXME: this can't be right, you want to call "json_deocde", not
# return it as a literal string in the header! (i.e. insert '. before json_decode and remove ' at the end)?
# All this code should be tested!
  header ('X-Taler-Contract-Hash: ' . json_decode($response)["H_contract"]);
  header ('X-Taler-Offer-Url: /donate');
  header ('X-Taler-Pay-Url: /pay'); ?>
 </body>
</html>

