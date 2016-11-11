# Check if a session exists already
session_start();
if (! isset($_SESSION['paid'])) @{
  echo "<p>There is no session for this purchase. Something is wrong.</p>";
  return;
@}
# Get the HTTP POST body
$payment = file_get_contents('php://input');
$response = post_to_backend("/pay", $payment);
if (200 != $response->getResponseCode())@{
  echo json_encode(array(
    'error' => "internal error",
    'hint' => "failed to POST coins to the backend",
    'detail' => $response->body->toString()
    ), JSON_PRETTY_PRINT);
  return;
@}
$_SESSION['paid'] = true;
return $response->body;

