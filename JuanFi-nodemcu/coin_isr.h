#pragma once

void disableCoinSlot() {
  coinSlotActive = false;
  digitalWrite(COIN_SET_PIN, LOW);
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
}

void activateCoinSlot() {
  digitalWrite(COIN_SET_PIN, HIGH);
  delay(200);
  noInterrupts();
  coin = 0;
  coinsChange = 0;
  pulsesThisSession = 0;
  droppedPulsesThisSession = 0;
  interrupts();
  processCoin = 0;
  acceptCoin = true;
  coinSlotActive = true;
  targetMilis = millis() + (unsigned long)MAX_WAIT_COIN_SEC;
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_ON));
}

