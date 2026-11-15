#pragma once

void disableCoinSlot() {
  coinSlotArming = false;
  coinSlotActive = false;
  acceptCoin = false;
  digitalWrite(COIN_SET_PIN, LOW);
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
}

void finishCoinSlotArm() {
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
  coinSlotArming = false;
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_ON));
}

void activateCoinSlot() {
  coinSlotArming = true;
  coinSlotActive = false;
  acceptCoin = false;
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
  digitalWrite(COIN_SET_PIN, HIGH);
  coinSlotArmAtMs = millis() + 200;
}

