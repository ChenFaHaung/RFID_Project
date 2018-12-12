import pandas as pd
import numpy as np
import keras
import matplotlib.pyplot as plt
from keras.models import Sequential
from keras.layers import Dense, Dropout, Activation, Flatten, Reshape, GlobalAveragePooling1D
from keras.layers import Conv2D, MaxPooling2D, Conv1D, MaxPooling1D

from sklearn import metrics
from sklearn.metrics import classification_report
from sklearn import preprocessing

import os
import math

def feature_normalize(dataset):

    mu = np.mean(dataset, axis=0)
    sigma = np.std(dataset, axis=0)
    return (dataset - mu)/sigma

def feature_to01(dataset):

    lo = np.amax(dataset, 0)
    mo = np.amin(dataset, 0)
    return (dataset - mo)/ (lo - mo)

batch_size = 32
num_classes = 10
epochs = 100
save_dir = os.path.join(os.getcwd(), 'saved_models')
model_name = 'keras_trained_model.h5'

# Data load
train = pd.read_csv("out.csv")
train_df = pd.DataFrame(train)

label = pd.read_csv("label.csv")
label_df = pd.DataFrame(label)

X_load = pd.read_csv("out_test.csv")
X_Prepare = pd.DataFrame(X_load)

Y_load = pd.read_csv("label_test.csv")
Y_Prepare = pd.DataFrame(Y_load)

train_arr = np.asarray(train_df, dtype= np.float32)
# train_arr = np.array(data_trsps)
Y_train = np.array(label_df)

# X_arr = np.array(test_trsps)
X_arr = np.asarray(X_Prepare, dtype= np.float32)
Y_test = np.array(Y_Prepare)

# eliminate the last row
train_arr = train_arr[0:-1]
X_arr = X_arr[0:-1]

# choose the part of the data
termin = 1000
train_arr = train_arr[100:termin]
X_arr = X_arr[100:termin]

# Normalization debug
train_arr = feature_normalize(train_arr)
X_arr = feature_normalize(X_arr)

#X_train = train_arr.reshape(int(Y_train.shape[0]), int(train_arr.shape[0]), 2)
#X_test = X_arr.reshape(int(Y_test.shape[0]), int(X_arr.shape[0]), 2)
X_train = np.hsplit(train_arr, 2)
X_test = np.hsplit(X_arr, 2)
X_train = np.asarray(X_train)
X_test = np.asarray(X_test)
# X_train = train_arr.reshape(int(Y_train.shape[0]), 2, int(train_arr.shape[1]))
print (X_train.shape)
num_samples, num_mode = X_train.shape[1], X_train.shape[2]
Train_input_shape = (num_samples * num_mode)
X_train = X_train.reshape(X_train.shape[0], Train_input_shape)
#print (X_train[0:5])
Test_shape = (X_test.shape[1] * X_test.shape[2])
X_test = X_test.reshape(X_test.shape[0], Test_shape)

X_train = X_train.astype("float32")
X_test = X_test.astype("float32")
Y_train = Y_train.astype("float32")
Y_train = Y_train.astype("float32")

# Convert class vectors to binary class matrices.  0 -> 1 0; 1 -> 0 1
y_train = keras.utils.to_categorical(Y_train)
y_test = keras.utils.to_categorical(Y_test)
#y_train = Y_train
#y_test = Y_test

#print(y_train)
# 1D CNN neural network
model_m = Sequential() 
model_m.add(Reshape((num_samples, num_mode), input_shape=(Train_input_shape,)))
model_m.add(Conv1D(100, 10, activation='relu', input_shape=(num_samples, num_mode)))
model_m.add(Conv1D(100, 10, activation='relu'))
model_m.add(MaxPooling1D(3))
model_m.add(Conv1D(160, 10, activation='relu'))
model_m.add(Conv1D(160, 10, activation='relu'))
model_m.add(GlobalAveragePooling1D())
model_m.add(Dropout(0.5))
model_m.add(Dense(2, activation='softmax'))
#model_m.add(Dense(units=1, kernel_initializer='normal', activation='sigmoid'))
print(model_m.summary())

callbacks_list = [
    keras.callbacks.ModelCheckpoint(
        filepath='best_model.{epoch:02d}-{val_loss:.2f}.h5',
        monitor='val_loss', save_best_only=True),
    keras.callbacks.EarlyStopping(monitor='acc', patience=5)
]
#model_m.compile(loss='binary_crossentropy',
#                optimizer='adam', metrics=['accuracy'])
model_m.compile(loss='categorical_crossentropy',
                optimizer='adam', metrics=['accuracy'])

# Hyper-parameters
BATCH_SIZE = 200
EPOCHS = 50

# Enable validation to use ModelCheckpoint and EarlyStopping callbacks.
history = model_m.fit(X_train,
                      y_train,
                      batch_size=BATCH_SIZE,
                      epochs=EPOCHS,
                      callbacks=callbacks_list,
                      validation_split=0.1,
                      verbose=1)

score = model_m.evaluate(X_test, y_test, verbose=1)

print("\nAccuracy on test data: %0.2f" % score[1])
print("\nLoss on test data: %0.2f" % score[0])

y_pred_test = model_m.predict(X_test)
# Take the class with the highest probability from the test predictions
max_y_pred_test = np.argmax(y_pred_test, axis=1)
max_y_test = np.argmax(y_test, axis=1)

plt.figure(figsize=(6, 4))
plt.plot(history.history['acc'], "g--", label="Accuracy of training data")
plt.plot(history.history['val_acc'], "g", label="Accuracy of validation data")
plt.plot(history.history['loss'], "r--", label="Loss of training data")
plt.plot(history.history['val_loss'], "r", label="Loss of validation data")
plt.title('Model Accuracy and Loss')
plt.ylabel('Accuracy and Loss')
plt.xlabel('Training Epoch')
plt.ylim(0)
plt.legend()
plt.show()

# print(classification_report(max_y_test, max_y_pred_test))
print (y_pred_test)

"""
classes = model.predict(X_test, batch_size=1)
predicted = np.reshape(classes, (classes.size,))
Q = np.array([])
for i in range(0,len(classes)):
    if classes[i][0]==0.0:
        Q = np.hstack((Q,int(1)))
    else:
        Q = np.hstack((Q,0))
#print(Q.astype(int))

print(classes)
print (Q)
plt.subplot(212)
plt.plot(Y_test)
plt.plot(Q)
plt.title('Test Result')
plt.ylabel('mode')
plt.xlabel('sample number')
plt.legend(['Origin', 'Predict'], loc='upper right')
plt.show()
"""