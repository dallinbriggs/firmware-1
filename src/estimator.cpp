#include "estimator.h"

namespace rosflight {

void Estimator::reset_state()
{
  q.w = 1.0f;
  q.x = 0.0f;
  q.y = 0.0f;
  q.z = 0.0f;
  omega.x = 0.0f;
  omega.y = 0.0f;
  omega.z = 0.0f;
  roll = 0.0f;
  pitch = 0.0f;
  yaw = 0.0f;

  q_hat.w = 1.0f;
  q_hat.x = 0.0f;
  q_hat.y = 0.0f;
  q_hat.z = 0.0f;

  w1.x = 0.0f;
  w1.y = 0.0f;
  w1.z = 0.0f;

  w2.x = 0.0f;
  w2.y = 0.0f;
  w2.z = 0.0f;

  b.x = 0.0f;
  b.y = 0.0f;
  b.z = 0.0f;

  w_acc.x = 0.0f;
  w_acc.y = 0.0f;
  w_acc.z = 0.0f;

  q_tilde.w = 1.0f;
  q_tilde.x = 0.0f;
  q_tilde.y = 0.0f;
  q_tilde.z = 0.0f;

  _accel_LPF.x = 0;
  _accel_LPF.y = 0;
  _accel_LPF.z = -9.80665;

  _gyro_LPF.x = 0;
  _gyro_LPF.y = 0;
  _gyro_LPF.z = 0;

  // Clear the unhealthy estimator flag
  _error_state &= ~(ERROR_UNHEALTHY_ESTIMATOR);
}

void Estimator::reset_adaptive_bias()
{
  b.x = 0;
  b.y = 0;
  b.z = 0;
}

void Estimator::init_estimator(Params* _params, Sensors* _sensors)
{
  params_ = _params;
  sensors_ = _sensors;
  mat_exp = false;
  quad_int = false;
  use_acc = true;

  last_time = 0;
  reset_state();
}

void Estimator::run_LPF()
{
  float alpha_acc = params_->get_param_float(PARAM_ACC_ALPHA);
  _accel_LPF.x = (1.0f-alpha_acc)*sensors_->_accel.x + alpha_acc*_accel_LPF.x;
  _accel_LPF.y = (1.0f-alpha_acc)*sensors_->_accel.y + alpha_acc*_accel_LPF.y;
  _accel_LPF.z = (1.0f-alpha_acc)*sensors_->_accel.z + alpha_acc*_accel_LPF.z;

  float alpha_gyro = params_->get_param_float(PARAM_GYRO_ALPHA);
  _gyro_LPF.x = (1.0f-alpha_gyro)*sensors_->_gyro.x + alpha_gyro*_gyro_LPF.x;
  _gyro_LPF.y = (1.0f-alpha_gyro)*sensors_->_gyro.y + alpha_gyro*_gyro_LPF.y;
  _gyro_LPF.z = (1.0f-alpha_gyro)*sensors_->_gyro.z + alpha_gyro*_gyro_LPF.z;
}


void Estimator::run_estimator()
{
  static float kp, ki;
  now_us = sensors_->_imu_time;
  if (last_time == 0 || now_us <= last_time)
  {
    last_time = now_us;
    return;
  }
  // clear the time going backwards error
  _error_state &= ~(ERROR_TIME_GOING_BACKWARDS);

  float dt = (now_us - last_time) * 1e-6f;
  last_time = now_us;

  // Crank up the gains for the first few seconds for quick convergence
  if (now_us < (uint64_t)params_->get_param_int(PARAM_INIT_TIME)*1000)
  {
    kp = params_->get_param_float(PARAM_FILTER_KP)*10.0f;
    ki = params_->get_param_float(PARAM_FILTER_KI)*10.0f;
  }
  else
  {
    kp = params_->get_param_float(PARAM_FILTER_KP);
    ki = params_->get_param_float(PARAM_FILTER_KI);
  }

  // Run LPF to reject a lot of noise
  run_LPF();

  // add in accelerometer
  float a_sqrd_norm = _accel_LPF.x*_accel_LPF.x + _accel_LPF.y*_accel_LPF.y + _accel_LPF.z*_accel_LPF.z;

  if (get_param_int(PARAM_FILTER_USE_ACC) && a_sqrd_norm < 1.15f*1.15f*9.80665f*9.80665f && a_sqrd_norm > 0.85f*0.85f*9.80665f*9.80665f)
  {
    // Keep track of the last time that the acc update ran
    last_acc_update_us = _current_state.now_us;
    // Get error estimated by accelerometer measurement
    vector_t a = vector_normalize(_accel_LPF);
    // Get the quaternion from accelerometer (low-frequency measure q)
    // (Not in either paper)
    quaternion_t q_acc_inv = quaternion_inverse(quat_from_two_vectors(a, g));
    // Get the error quaternion between observer and low-freq q
    // Below Eq. 45 Mahony Paper
    q_tilde = quaternion_multiply(q_acc_inv, q_hat);
    // Correction Term of Eq. 47a and 47b Mahony Paper
    // w_acc = 2*s_tilde*v_tilde
    w_acc.x = -2.0f*q_tilde.w*q_tilde.x;
    w_acc.y = -2.0f*q_tilde.w*q_tilde.y;
    w_acc.z = 0.0f; // Don't correct z, because it's unobservable from the accelerometer

    // integrate biases from accelerometer feedback
    // (eq 47b Mahony Paper, using correction term w_acc found above)
    b.x -= ki*w_acc.x*dt;
    b.y -= ki*w_acc.y*dt;
    b.z = 0.0;  // Don't integrate z bias, because it's unobservable
  }
  else
  {
    w_acc.x = 0.0f;
    w_acc.y = 0.0f;
    w_acc.z = 0.0f;
  }

  // Pull out Gyro measurements
  if (get_param_int(PARAM_FILTER_USE_QUAD_INT))
  {
    // Quadratic Integration (Eq. 14 Casey Paper)
    // this integration step adds 12 us on the STM32F10x chips
    wbar = vector_add(vector_add(scalar_multiply(-1.0f/12.0f,w2), scalar_multiply(8.0f/12.0f,w1)),
                      scalar_multiply(5.0f/12.0f,_gyro_LPF));
    w2 = w1;
    w1 = _gyro_LPF;
  }
  else
  {
    wbar = _gyro_LPF;
  }

  // Build the composite omega vector for kinematic propagation
  // This the stuff inside the p function in eq. 47a - Mahony Paper
  wfinal = vector_add(vector_sub(wbar, b), scalar_multiply(kp, w_acc));

  // Propagate Dynamics (only if we've moved)
  float sqrd_norm_w = sqrd_norm(wfinal);
  if (sqrd_norm_w > 0.0f)
  {
    float p = wfinal.x;
    float q = wfinal.y;
    float r = wfinal.z;

    if (get_param_int(PARAM_FILTER_USE_MAT_EXP))
    {
      // Matrix Exponential Approximation (From Attitude Representation and Kinematic
      // Propagation for Low-Cost UAVs by Robert T. Casey)
      // (Eq. 12 Casey Paper)
      // This adds 90 us on STM32F10x chips
      float norm_w = sqrt(sqrd_norm_w);
      quaternion_t qhat_np1;
      float t1 = cos((norm_w*dt)/2.0f);
      float t2 = 1.0f/norm_w * sin((norm_w*dt)/2.0f);
      qhat_np1.w = t1*q_hat.w   + t2*(- p*q_hat.x - q*q_hat.y - r*q_hat.z);
      qhat_np1.x = t1*q_hat.x   + t2*(p*q_hat.w             + r*q_hat.y - q*q_hat.z);
      qhat_np1.y = t1*q_hat.y   + t2*(q*q_hat.w - r*q_hat.x             + p*q_hat.z);
      qhat_np1.z = t1*q_hat.z   + t2*(r*q_hat.w + q*q_hat.x - p*q_hat.y);
      q_hat = quaternion_normalize(qhat_np1);
    }
    else
    {
      // Euler Integration
      // (Eq. 47a Mahony Paper), but this is pretty straight-forward
      quaternion_t qdot = {0.5f * (- p*q_hat.x - q*q_hat.y - r*q_hat.z),
                           0.5f * (p*q_hat.w             + r*q_hat.y - q*q_hat.z),
                           0.5f * (q*q_hat.w - r*q_hat.x             + p*q_hat.z),
                           0.5f * (r*q_hat.w + q*q_hat.x - p*q_hat.y)
                          };
      q_hat.w += qdot.w*dt;
      q_hat.x += qdot.x*dt;
      q_hat.y += qdot.y*dt;
      q_hat.z += qdot.z*dt;
      q_hat = quaternion_normalize(q_hat);
    }
  }

  // Save attitude estimate
  q = q_hat;

  // Extract Euler Angles for controller
  euler_from_quat(q, &roll, &pitch, &yaw);

  // Save off adjust gyro measurements with estimated biases for control
  omega = vector_sub(_gyro_LPF, b);
}

}