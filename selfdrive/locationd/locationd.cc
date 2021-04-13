#include "locationd.h"

using namespace EKFS;
using namespace Eigen;

ExitHandler do_exit;

IOFormat fmt(FullPrecision, 0, ", ", ",\n", "[", "]", "[", "]");
IOFormat fmt_row(FullPrecision, 0, ", ", ", ", "", "", "", "");

VectorXd floatlist2vector(const capnp::List<float, capnp::Kind::PRIMITIVE>::Reader& floatlist) {
  VectorXd res(floatlist.size());
  for (int i = 0; i < floatlist.size(); i++) {
    res[i] = floatlist[i];
  }
  return res;
}

VectorXd quat2vector(const Quaterniond& quat) {
  return Vector4d(quat.w(), quat.x(), quat.y(), quat.z());
}

Quaterniond vector2quat(const VectorXd& vec) {
  return Quaterniond(vec(0), vec(1), vec(2), vec(3));
}

void initMeasurement(cereal::LiveLocationKalman::Measurement::Builder meas, const VectorXd& val, const VectorXd& std, bool valid) {
  meas.setValue(kj::arrayPtr(val.data(), val.size()));
  meas.setStd(kj::arrayPtr(std.data(), std.size()));
  meas.setValid(valid);
}

Localizer::Localizer() {
  this->kf = std::make_shared<LiveKalman>();
  this->reset_kalman();

  this->calib = VectorXd(3);
  this->calib << 0.0, 0.0, 0.0;
  this->device_from_calib = MatrixXdr::Identity(3, 3);
  this->calib_from_device = MatrixXdr::Identity(3, 3);

  this->posenet_stds_old = VectorXd(POSENET_STD_HIST_HALF);
  this->posenet_stds_new = VectorXd(POSENET_STD_HIST_HALF);
  for (int i = 0; i < POSENET_STD_HIST_HALF; i++) {
    this->posenet_stds_old[i] = 10.0;
    this->posenet_stds_new[i] = 10.0;
  }

  VectorXd ecef_pos = this->kf->get_x().segment<STATE_ECEF_POS_LEN>(STATE_ECEF_POS_START);
  this->converter = std::make_shared<LocalCoord>((ECEF) { .x = ecef_pos[0], .y = ecef_pos[1], .z = ecef_pos[2] });

  if (std::getenv("PROCESS_REPLAY")) {
    this->send_on_all = true;
  }
}

void Localizer::liveLocationMsg(cereal::LiveLocationKalman::Builder& fix) {
  VectorXd predicted_state = this->kf->get_x();
  MatrixXdr predicted_cov = this->kf->get_P();
  VectorXd predicted_std = predicted_cov.diagonal().array().sqrt();

  VectorXd fix_ecef = predicted_state.segment<STATE_ECEF_POS_LEN>(STATE_ECEF_POS_START);
  VectorXd fix_ecef_std = predicted_std.segment<STATE_ECEF_POS_ERR_LEN>(STATE_ECEF_POS_ERR_START);
  VectorXd vel_ecef = predicted_state.segment<STATE_ECEF_VELOCITY_LEN>(STATE_ECEF_VELOCITY_START);
  VectorXd vel_ecef_std = predicted_std.segment<STATE_ECEF_VELOCITY_ERR_LEN>(STATE_ECEF_VELOCITY_ERR_START);
  Geodetic fix_pos_geo = ecef2geodetic((ECEF) { .x = fix_ecef(0), .y = fix_ecef(1), .z = fix_ecef(2) });
  VectorXd fix_pos_geo_vec = (VectorXd(3) << fix_pos_geo.lat, fix_pos_geo.lon, fix_pos_geo.alt).finished();
  //fix_pos_geo_std = np.abs(coord.ecef2geodetic(fix_ecef + fix_ecef_std) - fix_pos_geo)
  VectorXd orientation_ecef = quat2euler(vector2quat(predicted_state.segment<STATE_ECEF_ORIENTATION_LEN>(STATE_ECEF_ORIENTATION_START)));
  VectorXd orientation_ecef_std = predicted_std.segment<STATE_ECEF_ORIENTATION_ERR_LEN>(STATE_ECEF_ORIENTATION_ERR_START);
  MatrixXdr device_from_ecef = quat2rot(vector2quat(predicted_state.segment<STATE_ECEF_ORIENTATION_LEN>(STATE_ECEF_ORIENTATION_START))).transpose();
  VectorXd calibrated_orientation_ecef = rot2euler(this->calib_from_device * device_from_ecef);

  VectorXd acc_calib = this->calib_from_device * predicted_state.segment<STATE_ACCELERATION_LEN>(STATE_ACCELERATION_START);
  VectorXd acc_calib_std = ((this->calib_from_device * predicted_cov.block<STATE_ACCELERATION_ERR_LEN, STATE_ACCELERATION_ERR_LEN>(STATE_ACCELERATION_ERR_START, STATE_ACCELERATION_ERR_START)) * this->calib_from_device.transpose()).diagonal().array().sqrt();
  VectorXd ang_vel_calib = this->calib_from_device * predicted_state.segment<STATE_ANGULAR_VELOCITY_LEN>(STATE_ANGULAR_VELOCITY_START);

  MatrixXdr vel_angular_err = predicted_cov.block<STATE_ANGULAR_VELOCITY_ERR_LEN, STATE_ANGULAR_VELOCITY_ERR_LEN>(STATE_ANGULAR_VELOCITY_ERR_START, STATE_ANGULAR_VELOCITY_ERR_START);
  VectorXd ang_vel_calib_std = ((this->calib_from_device * vel_angular_err) * this->calib_from_device.transpose()).diagonal().array().sqrt();

  VectorXd vel_device = device_from_ecef * vel_ecef;
  VectorXd device_from_ecef_eul = quat2euler(vector2quat(predicted_state.segment<STATE_ECEF_ORIENTATION_LEN>(STATE_ECEF_ORIENTATION_START))).transpose();
  MatrixXdr condensed_cov(STATE_ECEF_ORIENTATION_ERR_LEN + STATE_ECEF_VELOCITY_ERR_LEN, STATE_ECEF_ORIENTATION_ERR_LEN + STATE_ECEF_VELOCITY_ERR_LEN);
  condensed_cov.topLeftCorner<STATE_ECEF_ORIENTATION_ERR_LEN, STATE_ECEF_ORIENTATION_ERR_LEN>() =
    predicted_cov.block<STATE_ECEF_ORIENTATION_ERR_LEN, STATE_ECEF_ORIENTATION_ERR_LEN>(STATE_ECEF_ORIENTATION_ERR_START, STATE_ECEF_ORIENTATION_ERR_START);
  condensed_cov.topRightCorner<STATE_ECEF_ORIENTATION_ERR_LEN, STATE_ECEF_VELOCITY_ERR_LEN>() =
    predicted_cov.block<STATE_ECEF_ORIENTATION_ERR_LEN, STATE_ECEF_VELOCITY_ERR_LEN>(STATE_ECEF_ORIENTATION_ERR_START, STATE_ECEF_VELOCITY_ERR_START);
  condensed_cov.bottomRightCorner<STATE_ECEF_VELOCITY_ERR_LEN, STATE_ECEF_VELOCITY_ERR_LEN>() =
    predicted_cov.block<STATE_ECEF_VELOCITY_ERR_LEN, STATE_ECEF_VELOCITY_ERR_LEN>(STATE_ECEF_VELOCITY_ERR_START, STATE_ECEF_VELOCITY_ERR_START);
  condensed_cov.bottomLeftCorner<STATE_ECEF_VELOCITY_ERR_LEN, STATE_ECEF_ORIENTATION_ERR_LEN>() =
    predicted_cov.block<STATE_ECEF_VELOCITY_ERR_LEN, STATE_ECEF_ORIENTATION_ERR_LEN>(STATE_ECEF_VELOCITY_ERR_START, STATE_ECEF_ORIENTATION_ERR_START);
  VectorXd H_input(device_from_ecef_eul.size() + vel_ecef.size());
  H_input << device_from_ecef_eul, vel_ecef;
  MatrixXdr HH = this->kf->H(H_input);
  MatrixXdr vel_device_cov = (HH * condensed_cov) * HH.transpose();
  VectorXd vel_device_std = vel_device_cov.diagonal().array().sqrt();

  VectorXd vel_calib = this->calib_from_device * vel_device;
  VectorXd vel_calib_std = ((this->calib_from_device * vel_device_cov) * this->calib_from_device.transpose()).diagonal().array().sqrt();

  ECEF fix_ecef_ecef = (ECEF) { .x = fix_ecef(0), .y = fix_ecef(1), .z = fix_ecef(2) };
  VectorXd orientation_ned = ned_euler_from_ecef(fix_ecef_ecef, orientation_ecef);
  //orientation_ned_std = ned_euler_from_ecef(fix_ecef, orientation_ecef + orientation_ecef_std) - orientation_ned
  VectorXd nextfix_ecef = fix_ecef + vel_ecef;
  VectorXd ned_vel = this->converter->ecef2ned((ECEF) { .x = nextfix_ecef(0), .y = nextfix_ecef(1), .z = nextfix_ecef(2) }).to_vector() - converter->ecef2ned(fix_ecef_ecef).to_vector();
  //ned_vel_std = self.converter->ecef2ned(fix_ecef + vel_ecef + vel_ecef_std) - self.converter->ecef2ned(fix_ecef + vel_ecef)

  VectorXd accDevice = predicted_state.segment<STATE_ACCELERATION_LEN>(STATE_ACCELERATION_START);
  VectorXd accDeviceErr = predicted_std.segment<STATE_ACCELERATION_ERR_LEN>(STATE_ACCELERATION_ERR_START);

  VectorXd angVelocityDevice = predicted_state.segment<STATE_ANGULAR_VELOCITY_LEN>(STATE_ANGULAR_VELOCITY_START);
  VectorXd angVelocityDeviceErr = predicted_std.segment<STATE_ANGULAR_VELOCITY_ERR_LEN>(STATE_ANGULAR_VELOCITY_ERR_START);

  VectorXd nans = (VectorXd(3) << NAN, NAN, NAN).finished();

  // write measurements to msg
  // TODO initMeasurement(fix.initPositionGeodetic(), fix_pos_geo_vec, nans, true);
  auto positionGeodetic = fix.initPositionGeodetic();
  positionGeodetic.setValue(kj::arrayPtr(fix_pos_geo_vec.data(), 3));
  positionGeodetic.setStd(kj::arrayPtr(nans.data(), 3));
  positionGeodetic.setValid(true);

  auto positionECEF = fix.initPositionECEF();
  positionECEF.setValue(kj::arrayPtr(fix_ecef.data(), 3));
  positionECEF.setStd(kj::arrayPtr(fix_ecef_std.data(), 3));
  positionECEF.setValid(true);

  auto velocityECEF = fix.initVelocityECEF();
  velocityECEF.setValue(kj::arrayPtr(vel_ecef.data(), 3));
  velocityECEF.setStd(kj::arrayPtr(vel_ecef_std.data(), 3));
  velocityECEF.setValid(true);

  auto velocityNED = fix.initVelocityNED();
  velocityNED.setValue(kj::arrayPtr(ned_vel.data(), 3));
  velocityNED.setStd(kj::arrayPtr(nans.data(), 3));
  velocityNED.setValid(true);

  auto velocityDevice = fix.initVelocityDevice();
  velocityDevice.setValue(kj::arrayPtr(vel_device.data(), 3));
  velocityDevice.setStd(kj::arrayPtr(vel_device_std.data(), 3));
  velocityDevice.setValid(true);

  auto accelerationDevice = fix.initAccelerationDevice();
  accelerationDevice.setValue(kj::arrayPtr(accDevice.data(), 3));
  accelerationDevice.setStd(kj::arrayPtr(accDeviceErr.data(), 3));
  accelerationDevice.setValid(true);

  auto orientationECEF = fix.initOrientationECEF();
  orientationECEF.setValue(kj::arrayPtr(orientation_ecef.data(), 3));
  orientationECEF.setStd(kj::arrayPtr(orientation_ecef_std.data(), 3));
  orientationECEF.setValid(true);

  auto calibratedOrientationECEF = fix.initCalibratedOrientationECEF();
  calibratedOrientationECEF.setValue(kj::arrayPtr(calibrated_orientation_ecef.data(), 3));
  calibratedOrientationECEF.setStd(kj::arrayPtr(nans.data(), 3));
  calibratedOrientationECEF.setValid(this->calibrated);

  auto orientationNED = fix.initOrientationNED();
  orientationNED.setValue(kj::arrayPtr(orientation_ned.data(), 3));
  orientationNED.setStd(kj::arrayPtr(nans.data(), 3));
  orientationNED.setValid(true);

  auto angularVelocityDevice = fix.initAngularVelocityDevice();
  angularVelocityDevice.setValue(kj::arrayPtr(angVelocityDevice.data(), 3));
  angularVelocityDevice.setStd(kj::arrayPtr(angVelocityDeviceErr.data(), 3));
  angularVelocityDevice.setValid(true);

  auto velocityCalibrated = fix.initVelocityCalibrated();
  velocityCalibrated.setValue(kj::arrayPtr(vel_calib.data(), 3));
  velocityCalibrated.setStd(kj::arrayPtr(vel_calib_std.data(), 3));
  velocityCalibrated.setValid(this->calibrated);

  auto angularVelocityCalibrated = fix.initAngularVelocityCalibrated();
  angularVelocityCalibrated.setValue(kj::arrayPtr(ang_vel_calib.data(), 3));
  angularVelocityCalibrated.setStd(kj::arrayPtr(ang_vel_calib_std.data(), 3));
  angularVelocityCalibrated.setValid(this->calibrated);

  auto accelerationCalibrated = fix.initAccelerationCalibrated();
  accelerationCalibrated.setValue(kj::arrayPtr(acc_calib.data(), 3));
  accelerationCalibrated.setStd(kj::arrayPtr(acc_calib_std.data(), 3));
  accelerationCalibrated.setValid(this->calibrated);

  // experimentally found these values, no false positives in 20k minutes of driving
  double old_mean = this->posenet_stds_old.mean();
  double new_mean = this->posenet_stds_new.mean();
  bool std_spike = (new_mean / old_mean > 4.0 && new_mean > 7.0);

  fix.setPosenetOK(!(std_spike && this->car_speed > 5.0));
  fix.setDeviceStable(!this->device_fell);
  this->device_fell = false;

  //fix.setGpsWeek(this->time.week);
  //fix.setGpsTimeOfWeek(this->time.tow);
  fix.setUnixTimestampMillis(this->unix_timestamp_millis);

  if (fix_ecef_std.norm() < 50.0 && this->calibrated) {
    fix.setStatus(cereal::LiveLocationKalman::Status::VALID);
  } else if (fix_ecef_std.norm() < 50.0) {
    fix.setStatus(cereal::LiveLocationKalman::Status::UNCALIBRATED);
  } else {
    fix.setStatus(cereal::LiveLocationKalman::Status::UNINITIALIZED);
  }
}

void Localizer::update_kalman(double t, int kind, std::vector<VectorXd> meas, std::vector<MatrixXdr> R) {
  try {
    this->kf->predict_and_observe(t, kind, meas, R);
  }
  catch (std::exception e) {  // TODO specify exception
    std::cout << "Error in predict and observe, kalman reset" << std::endl;  // TODO cloudlog
    this->reset_kalman();
  }
}

void Localizer::handle_sensors(double current_time, const capnp::List<cereal::SensorEventData, capnp::Kind::STRUCT>::Reader& log) {
  // TODO does not yet account for double sensor readings in the log
  for (int i = 0; i < log.size(); i++) {
    const cereal::SensorEventData::Reader& sensor_reading = log[i];
    double sensor_time = 1e-9 * sensor_reading.getTimestamp();
    // TODO: handle messages from two IMUs at the same time
    if (sensor_reading.getSource() == cereal::SensorEventData::SensorSource::LSM6DS3) {
      continue;
    }

    // Gyro Uncalibrated
    if (sensor_reading.getSensor() == 5 && sensor_reading.getType() == 16) {
      this->gyro_counter++;
      if (this->gyro_counter % SENSOR_DECIMATION == 0) {
        auto v = sensor_reading.getGyroUncalibrated().getV();
        this->update_kalman(sensor_time, KIND_PHONE_GYRO, { (VectorXd(3) << -v[2], -v[1], -v[0]).finished() });
      }
    }

    // Accelerometer
    if (sensor_reading.getSensor() == 1 && sensor_reading.getType() == 1) {
      auto v = sensor_reading.getAcceleration().getV();

      // check if device fell, estimate 10 for g
      // 40m/s**2 is a good filter for falling detection, no false positives in 20k minutes of driving
      this->device_fell |= (floatlist2vector(v) - (VectorXd(3) << 10.0, 0.0, 0.0).finished()).norm() > 40;

      this->acc_counter++;
      if (this->acc_counter % SENSOR_DECIMATION == 0) {
        this->update_kalman(sensor_time, KIND_PHONE_ACCEL, { (VectorXd(3) << -v[2], -v[1], -v[0]).finished() });
      }
    }
  }
}

void Localizer::handle_gps(double current_time, const cereal::GpsLocationData::Reader& log) {
  // ignore the message if the fix is invalid
  if (log.getFlags() % 2 == 0) {
    return;
  }

  this->last_gps_fix = current_time;

  Geodetic geodetic = { log.getLatitude(), log.getLongitude(), log.getAltitude() };
  this->converter = std::make_shared<LocalCoord>(geodetic);

  VectorXd ecef_pos = this->converter->ned2ecef({ 0.0, 0.0, 0.0 }).to_vector();
  VectorXd ecef_vel = this->converter->ned2ecef({ log.getVNED()[0], log.getVNED()[1], log.getVNED()[2] }).to_vector() - ecef_pos;
  double vertical_accuracy = std::pow(3.0 * log.getVerticalAccuracy(), 2);
  MatrixXdr ecef_pos_R = (Vector3d() << vertical_accuracy, vertical_accuracy, vertical_accuracy).finished().asDiagonal();
  double speed_accuracy = std::pow(log.getSpeedAccuracy(), 2);
  MatrixXdr ecef_vel_R = (Vector3d() << speed_accuracy, speed_accuracy, speed_accuracy).finished().asDiagonal();

  this->unix_timestamp_millis = log.getTimestamp();
  double gps_est_error = (this->kf->get_x().head(3) - ecef_pos).norm();

  VectorXd orientation_ecef = quat2euler(vector2quat(this->kf->get_x().segment<STATE_ECEF_ORIENTATION_LEN>(STATE_ECEF_ORIENTATION_START)));
  VectorXd orientation_ned = ned_euler_from_ecef({ ecef_pos(0), ecef_pos(1), ecef_pos(2) }, orientation_ecef);
  VectorXd orientation_ned_gps = (VectorXd(3) << 0.0, 0.0, DEG2RAD(log.getBearingDeg())).finished();
  VectorXd orientation_error = (orientation_ned - orientation_ned_gps).array() - M_PI;
  for (int i = 0; i < orientation_error.size(); i++) {
    orientation_error(i) = std::fmod(orientation_error(i), 2.0 * M_PI);
    if (orientation_error(i) < 0.0) {
      orientation_error(i) += 2.0 * M_PI;
    }
    orientation_error(i) -= M_PI;
  }
  VectorXd initial_pose_ecef_quat = quat2vector(euler2quat(ecef_euler_from_ned({ ecef_pos(0), ecef_pos(1), ecef_pos(2) }, orientation_ned_gps)));

  if (ecef_vel.norm() > 5.0 && orientation_error.norm() > 1.0) {
    std::cout << "Locationd vs ubloxLocation orientation difference too large, kalman reset" << std::endl;
    this->reset_kalman(NAN, initial_pose_ecef_quat, ecef_pos);
    this->update_kalman(current_time, KIND_ECEF_ORIENTATION_FROM_GPS, { initial_pose_ecef_quat });
  } else if (gps_est_error > 50.0) {
    std::cout << "Locationd vs ubloxLocation position difference too large, kalman reset" << std::endl;
    this->reset_kalman(NAN, initial_pose_ecef_quat, ecef_pos);
  }

  this->update_kalman(current_time, KIND_ECEF_POS, { ecef_pos }, { ecef_pos_R });
  this->update_kalman(current_time, KIND_ECEF_VEL, { ecef_vel }, { ecef_vel_R });
}


void Localizer::handle_car_state(double current_time, const cereal::CarState::Reader& log) {
  this->speed_counter++;

  if (this->speed_counter % SENSOR_DECIMATION == 0) {
    this->update_kalman(current_time, KIND_ODOMETRIC_SPEED, { (VectorXd(1) << log.getVEgo()).finished() });
    this->car_speed = abs(log.getVEgo());
    if (log.getVEgo() == 0.0) {
      this->update_kalman(current_time, KIND_NO_ROT, { (VectorXd(3) << 0.0, 0.0, 0.0).finished() });
    }
  }
}

void Localizer::handle_cam_odo(double current_time, const cereal::CameraOdometry::Reader& log) {
  this->cam_counter++;

  if (this->cam_counter % VISION_DECIMATION == 0) {
    // TODO len of vectors is always 3
    VectorXd rot_device = this->device_from_calib * floatlist2vector(log.getRot());
    VectorXd rot_device_std = (this->device_from_calib * floatlist2vector(log.getRotStd())) * 10.0;
    this->update_kalman(current_time, KIND_CAMERA_ODO_ROTATION,
      { (VectorXd(rot_device.rows() + rot_device_std.rows()) << rot_device, rot_device_std).finished() });

    VectorXd trans_device = this->device_from_calib * floatlist2vector(log.getTrans());
    VectorXd trans_device_std = this->device_from_calib * floatlist2vector(log.getTransStd());

    this->posenet_stds_old[this->posenet_stds_i] = this->posenet_stds_new[this->posenet_stds_i];
    this->posenet_stds_new[this->posenet_stds_i] = trans_device_std[0];
    this->posenet_stds_i = (this->posenet_stds_i + 1) % POSENET_STD_HIST_HALF;

    trans_device_std *= 10.0;
    this->update_kalman(current_time, KIND_CAMERA_ODO_TRANSLATION,
      { (VectorXd(trans_device.rows() + trans_device_std.rows()) << trans_device, trans_device_std).finished() });
  }
}

void Localizer::handle_live_calib(double current_time, const cereal::LiveCalibrationData::Reader& log) {
  if (log.getRpyCalib().size() > 0) {
    this->calib = floatlist2vector(log.getRpyCalib());
    this->device_from_calib = euler2rot(this->calib);
    this->calib_from_device = this->device_from_calib.transpose();
    this->calibrated = log.getCalStatus() == 1;
  }
}

void Localizer::reset_kalman(double current_time) {  // TODO nan ?
  VectorXd init_x = this->kf->get_initial_x();
  this->reset_kalman(current_time, init_x.segment<4>(3), init_x.head(3));
}

void Localizer::reset_kalman(double current_time, VectorXd init_orient, VectorXd init_pos) {
  // too nonlinear to init on completely wrong
  VectorXd init_x = this->kf->get_initial_x();
  MatrixXdr init_P = this->kf->get_initial_P();
  init_x.segment<4>(3) = init_orient;
  init_x.head(3) = init_pos;

  this->kf->init_state(init_x, init_P, current_time);

  this->gyro_counter = 0;
  this->acc_counter = 0;
  this->speed_counter = 0;
  this->cam_counter = 0;
}

int Localizer::locationd_thread() {
  const std::initializer_list<const char *> service_list =
      { "gpsLocationExternal", "sensorEvents", "cameraOdometry", "liveCalibration", "carState" };
  SubMaster sm(service_list, nullptr, { "gpsLocationExternal" });
  PubMaster pm({ "liveLocationKalman" });

  Params params;

  std::ofstream myfile;
  myfile.open("test_locationd_cpp_out.txt", std::ios::out | std::ios::trunc);

  while (!do_exit) {
    bool updatedCameraOdometry = false;
    sm.update(); // TODO timeout?
    for (const char* service : service_list) {
      if (sm.updated(service) && sm.valid(service)) {
        cereal::Event::Reader& log = sm[service];
        double t = log.getLogMonoTime() * 1e-9;

        if (log.isSensorEvents()) {
          this->handle_sensors(t, log.getSensorEvents());
        } else if (log.isGpsLocationExternal()) {
          this->handle_gps(t, log.getGpsLocationExternal());
        } else if (log.isCarState()) {
          this->handle_car_state(t, log.getCarState());
        } else if (log.isCameraOdometry()) {
          this->handle_cam_odo(t, log.getCameraOdometry());
          updatedCameraOdometry = true;
        } else if (log.isLiveCalibration()) {
          this->handle_live_calib(t, log.getLiveCalibration());
        } else {
          std::cout << "invalid event" << std::endl;
        }
        myfile << this->kf->get_x().format(fmt_row) << std::endl;
      }
    }

    if (updatedCameraOdometry) {
      double t = sm["cameraOdometry"].getLogMonoTime();

      MessageBuilder msg_builder;
      auto evt = msg_builder.initEvent();
      evt.setLogMonoTime(t);
      auto liveLoc = evt.initLiveLocationKalman();
      this->liveLocationMsg(liveLoc);
      liveLoc.setInputsOK(sm.allAliveAndValid());
      liveLoc.setSensorsOK(sm.alive("sensorEvents") && sm.valid("sensorEvents"));
      liveLoc.setGpsOK((t / 1e9) - this->last_gps_fix < 1.0);
      pm.send("liveLocationKalman", msg_builder);

// TODO:
//       if sm.frame % 1200 == 0 and msg.liveLocationKalman.gpsOK:  # once a minute
//         location = {
//           'latitude': msg.liveLocationKalman.positionGeodetic.value[0],
//           'longitude': msg.liveLocationKalman.positionGeodetic.value[1],
//           'altitude': msg.liveLocationKalman.positionGeodetic.value[2],
//         }
//         params.put("LastGPSPosition", json.dumps(location))

    } else if (this->send_on_all) {
      MessageBuilder msg_builder;
      msg_builder.initEvent();
      pm.send("liveLocationKalman", msg_builder);
    }
  }
  return 0;
}

int main() {
  Localizer localizer;
  return localizer.locationd_thread();
}